# Present Interposers (NVIDIA Smooth Motion)

Last verified: 2026-09-14 (build 0.1.6559, Strange Brigade DX12 sessions `20260914_102700` / `105242` / `105853`.
The output-chain topology below is NOT yet hardware-confirmed: the app-facing fallback was, in 0.1.6555/0.1.6556.)

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
2. **CE composites on the interposer's output chain, and submits that overlay on THAT CHAIN'S OWN QUEUE.** The
   back buffers belong to the queue that created the chain. Submitting on the application's queue instead is
   cross-queue backbuffer access, which removes the device with `DXGI_ERROR_ACCESS_DENIED` (0x887A002B) — the exact
   HRESULT `dx12_overlay_policy/ffx_routing.h` already records for the late-inject DLSS-G case, and the same rule as
   `kUseFSRSwapchainQueue`. **The chain was never the problem; the queue was.** Where CE never observed the create,
   there is no safe queue and CE must not draw there at all — it falls back to the application-facing chain.
3. **A swapchain created by an interposer is that interposer's private chain, permanently.** Unlike an overlay —
   which can legitimately wrap the game's own authoritative create and make the real game swapchain look foreign —
   an interposer's create is always in *addition* to the application's. No later "the game presents on it after all"
   evidence may reclassify it. The one correction that does apply: the object handed back to the application is by
   definition not private, so `AssignCreatedSwapchain` unregisters it. That covers an interposer that merely
   forwards the application's create unchanged.
4. **The application's present stream is 1x under an interposer**, however many frames the driver generates.
5. **The swapchain wrapper delegates while the detour composites on the output chain, and only then.** Delegation
   makes the wrapper a pure pass-through so the frame is composited exactly once. When there is no compositable
   output chain, the wrapper must NOT delegate: the detour would deliberately pass the frame through and the overlay
   would never be composited at all.
6. **Everything above the interposer stays untouched.** No present-mode override, no interval override, no extra
   GPU work on the application's queue. The generated group must arrive at DXGI already spread by the driver's own
   metering; CE's only contract is stated on the final flip (see Forced vsync).

## Where CE's overlay goes
On the interposer's **output** chain, through the deep `dxgi!Present` body hook, submitted on that chain's own queue.
That placement is what gives both properties at once:

- **Topmost.** CE's body hook sits below the dxgi Present entry, so Steam and RTSS — which patch that entry — have
  already drawn by the time CE runs. Last to composite is topmost.
- **Not interpolated.** The driver has already generated the frame by the time it presents this chain, so CE's
  overlay goes onto the displayed frame, not into the interpolator's input. This is where RTSS draws too.
- The overlay is drawn on every displayed frame, real and generated alike.

The application-facing wrapper stays a pure pass-through in this mode (`ShouldDelegateDX12PresentToDetourHook`
returns true), so the frame is composited exactly once. It still counts the application's presents for the cadence
measurement.

**Fallback**: if CE never observed the interposer's create — so it has no queue for that chain — it wraps the
application-facing swapchain and composites there instead. That overlay IS interpolated and draws below Steam/RTSS.
It is the safe state, not the intended one.

### DX11/DX10: the same chain, a different constraint

There is no command queue in D3D11, so the queue rule above has nothing to say and a null queue must **not** be read
as "no safe route". The chain's own device is reachable through `IDXGISwapChain::GetDevice`, and drawing with it onto
its own back buffer is exactly what an overlay is supposed to do. What changes is **retention**:

- CE hooks an interposer's private chain `presentOnly` (slot 8/22), so slot 13 is not CE's and the interposer's own
  `ResizeBuffers` is invisible to CE. A retained RTV therefore pins a back buffer the interposer recreates on its own
  schedule, with nothing to tell CE to drop it first — unlike the application's chain, where `DetourResizeBuffers`
  runs `CleanupDX11Resources` before the resize.
- So on an interposer's private chain the back-buffer RTV is built and released **inside the one Present**
  (`PresentInterposerCompositeRoute::kOutputChainTransientBackbuffer`). Nothing CE creates there outlives the call.
- And the render target the interposer left bound at Present entry is never adopted as the overlay target. On the
  application's chain that bound target is the frame the game just finished; on the interposer's chain it is one of
  the interpolator's own intermediates, so drawing there writes the overlay into driver-internal state instead of
  onto the frame.

`ce::overlay_compat::ResolvePresentInterposerCompositeRoute` answers this for every API, in
`ExecutePresentCore` **before** the D3D12 branch, and `DXGIShared::IsPresentOnPresentInterposerPrivateOutputChain`
carries the per-Present, per-thread answer down to the DX11/DX10 overlay.

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
- `DetourCreateSwapChainGlobal: Present interposer <path> created its output swapchain <ptr> on queue <ptr>` — the
  create was classified AND the queue recorded. Absent under Smooth Motion means the caller-module test missed; a
  null queue there means the chain is not compositable and the fallback will engage.
- `CreateSwapChain: A present interposer implements Present for the app-facing swapchain (sc=...)` — CE took the
  wrapper instead of preserving identity.
- `SwapChain: Present for real=... is implemented above dxgi (present interposer)` — the wrapper knows a present
  interposer owns this object's Present.
- `DXGI: stating the vertical blank on the present interposer's own flip (sc=... sync=0->1 flags=0x200->0x0)` —
  forced FIFO reaching the final flip.
- `DX12: ProcessFrame — present interposer output chain <ptr>, using its own queue <ptr>` — invariant 2 holding.
  A `path=primaryQ`/`origGame` line for an interposer chain instead means the queue was lost: that is the crash.
- `DetourPresent: Present interposer output swapchain <ptr> has no observed queue` — the fallback engaged.
- `DetourPresent: Present interposer output swapchain <ptr> composites through the transient back-buffer route` —
  the DX11/DX10 route above. Its absence in a DX11 Smooth Motion session means the classification missed.
- `DX11: Creating transient RTV for SwapChain <ptr>` / `DX11: Releasing the retained swapchain RTV — this Present is
  on a present interposer's private chain` — retention actually dropped. A `Creating retained RTV` line for a chain
  the create log called the interposer's is the regression.
- `DetourPresent: Present interposer output cadence window #N — application=... output=... generating=... multiplier=...`
- **Device removed with `DXGI_ERROR_ACCESS_DENIED` (`0x887A002B`) out of `GetDeviceRemovedReason()` right after CE's
  first overlay `ExecuteCommandLists`, with no TDR in the Windows System log** — CE is drawing into one queue's
  backbuffers while submitting on another. That is this page's founding bug (session `20260914_102700`): the game
  then crashed on a null dereference 1.1 s later, after CE started swallowing its command lists.
- **The game exits with `0xC0000409` and NvPresent64.dll as the faulting module, with no CE dump and no CE frames on
  the faulting stack** — CE composited on the interposer's private chain with no route policy. Witcher 3 DX11,
  session `20260919_154534` (both runs, identical offset `NvPresent64+0x1b6fd1`): that address is `int 29h` preceded
  by `mov ecx, 7`, i.e. `__fastfail(FAST_FAIL_FATAL_APP_EXIT)` from the UCRT's `abort()`, called by NvPresent64's own
  `std::terminate` — an unhandled C++ exception inside the interposer, about 1.4 s after CE's first overlay frame.
  The exit code is the giveaway that no CE handler could have run: see `regression-testing-and-logging.md`.

## Forced vsync
**`vsync_mode=fifo` is stated on the interposer's own output flip**, and nowhere else. This is the Portal RTX result
of 2026-09-13 restated for DXGI, and it cuts the opposite way from the obvious reading of that investigation:

- Forcing the override at the interposer's *input* (the application-facing present) is above the generator. It is
  also useless — NvPresent64 does not propagate it, and its output present was observed as `SyncInterval=0
  Flags=512 (DXGI_PRESENT_ALLOW_TEARING)`, which IS its flip scheduling.
- Forcing it on the *output* flip is the validated contract. `vulkan_dxgi_fifo_policy.h` spells out why the earlier
  "forcing FIFO unpaces a metered generator" conclusion was measured on the wrong thing: those sessions forced the
  Vulkan present MODE as well, and that is what collapsed NVIDIA's announced flip lead from 6842 us to 141 us.
  Quantizing an already-correctly-spread group onto vertical blanks is vertical-blank synchronization; quantizing an
  unpaced burst was the judder.

CE therefore reuses the same pure contract, `ce::vulkan_dxgi_fifo_policy::ApplyFinalDxgiFifoParameters`:
`SyncInterval=1` with `ALLOW_TEARING`, `RESTART` and `DO_NOT_WAIT` cleared, on the interposer's output present only.
`off` and `mailbox` are not a vertical-blank contract and are left alone — the interposer's own parameters already
are those. Nothing above the generator is touched, no timer is added and no driver profile is written.

## Rejected approaches (do NOT re-pursue)
- Compositing on the interposer's output chain using the APPLICATION's queue. That is the founding bug.
- Keeping the overlay on the application-facing chain as the normal mode. It works, but the overlay is then
  interpolated and draws below Steam/RTSS; it is the fallback for a chain whose queue CE never saw.
- Forcing the present MODE, or any interval, ABOVE the interposer. That is what unpaces a metered generator.
- Restoring the old Smooth Motion heuristics by putting CE back on the driver's private chain.
- Retaining ANY resource built from an interposer's private chain across Presents, in any API.
- Adopting the render target bound at Present entry as the overlay target on an interposer's private chain.
- Deciding the interposer route inside a per-API branch. It is a property of who owns the chain, not of the API; the
  Witcher 3 crash below is what that mistake cost.

## Open questions / stale-risk
- Stale-risk **medium**: the classification is keyed on the `nvpresent` module name. A driver that renames or
  relocates the interposer, or a different vendor shipping the same topology, needs the token table extended.
- **The output-chain topology has not been run on hardware yet.** 0.1.6555/0.1.6556 validated the app-facing
  fallback (`20260914_105853`); 0.1.6557 moves the overlay to the output chain on the interposer's queue. Watch for
  `devRemoved=0x887A002B` on the first `Reinit SUBMIT`: if it returns, the queue was not the whole story and the
  next suspects are the backbuffer resource state NvPresent64 leaves before Present, and whether it tracks
  submissions on its own queue.
- The visible `NVIDIA SM` label from the cadence path has not been confirmed on hardware either.
- DX11 Smooth Motion **does** have the same proxy topology — confirmed on hardware, Witcher 3, session
  `20260919_154534`: NvPresent64 created two private output chains on the real factory and CE's only Present view
  was those chains, never the application's. CE's overlay device there (`0000022DD76731C0`) is NvPresent64's own
  D3D11 device, reached through `GetDevice` on the private chain, not the game's (`0000022DB68BF0D0`).
  **Fix not yet hardware-validated.**
- Still open in DX11: the overlay device/context are cached from the FIRST chain CE draws on, while Smooth Motion
  creates TWO private chains on two different devices. A Present on the second chain would build its RTV with the
  first chain's device. Not observed (only chain #1 ever presented that session), not ruled out.
- Vulkan Smooth Motion still relies on the older heuristics and the invisible-window guards
  (`ShouldSkipWindowForNvPresent`); whether that path has the same proxy topology is unverified.
- CE's factory wrapper is what sees the application-facing create. A game that reaches the real DXGI factory without
  it would leave CE with no app-facing view at all under an interposer; not observed, but not ruled out.
