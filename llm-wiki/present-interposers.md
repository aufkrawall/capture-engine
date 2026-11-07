# Present Interposers (NVIDIA Smooth Motion)

Last verified: 2026-11-06 (worker-run status VALIDATED in Trine 5 DX11, session `20261106_181807`, 0.1.7435. The limiter's
output slots below have not run on hardware. Strange Brigade DX12 sessions `20260914_102700` / `105242` / `105853` for the
DX12 topology, whose output-chain route is NOT yet hardware-confirmed: the app-facing fallback was, in 0.1.6555/0.1.6556.)

Primary sources:
- `hook/overlay/overlay_compat_detail/module_table.h` (`IsPresentInterposerModulePath` and friends)
- `hook/overlay/overlay_compat_detail/routing_policy.h`
- `hook/present/present_interposer_evidence.h` (which evidence, the worker runs, the status latch)
- `hook/present/present_interposer_cadence.h`
- `hook/present/present_interposer_tracking.cpp`
- `hook/fg/fg_interposer_status.cpp` (status and rates, both DLLs)
- `hook/pacing/interposer_output_pacing.h`, `hook/pacing/fps_limiter_detail/interposer_output.h` (the limiter)
- `hook/present/dxgi_shared.cpp` (`IsSwapchainPresentCoveredByDeepBodyHook`, the private-chain registry)
- `hook/present/dxgi_shared_present.cpp`, `dxgi_shared_present1.cpp`, `dxgi_shared_present_core.cpp`
- `hook/d3d12/dx12_hook_swapchain_create.cpp`, `dx12_hook_swapchain_wrap_policy.cpp`
- `hook/wrappers/dxgi_factory_wrap.cpp`, `dxgi_swapchain_wrap_present.cpp`, `dxgi_swapchain_wrap_internal.h`
- `tests/test_present_interposer_evidence.cpp`, `tests/test_smooth_motion_rates.cpp`,
  `tests/test_present_interposer_cadence.cpp`, `tests/test_overlay_compat.cpp`, `tests/test_dxgi_shared_part11.cpp`,
  `tests/test_fps_limiter_interposer_output.cpp`

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

A frame-generation runtime that presents its own swapchain (Intel XeFG, 2026-10-23) follows the same placement rule
(composite on the output chain, on that chain's own queue) through `ce::fg_presenter` and the runtime-output route;
its attribution, queue rebinding and limiter site differ: `frame-generation/xefg.md`.

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
   metering; CE's only contract is stated on the final flip (see Forced vsync). The FPS limiter's wait on the game thread
   is the one thing CE does there: it delays a call and changes nothing about it.
7. **The interposer's own pipeline carries no CE wrappers.** `IsGraphicsImportHookTargetModulePath` keeps NvPresent out
   of the import patches, so its `D3D11CreateDevice` and `GetProcAddress` are the real ones. A wrapped device there runs
   its draws and CE's overlay through a wrapper context, where they looked like the game's (Trine 5, below), and forced
   AF has no business on it.

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
The generation factor is the interposer's output presents against the application's frames. Output presents are
counted at the top of `DetourPresent`/`DetourPresent1`, before every early return, or at the top of the Vulkan layer's
present. `FGCompatibility::NoteInterposerOutputPresent` owns everything from there; 1:1 forwarding is Smooth Motion
loaded and NOT engaged, and publishes as no frame generation rather than as a 1x generator.

### Where the application's frames come from

`ce::present_interposer::InterposerEvidence` uses the best of three, and the window line names which
(`stream=application-presents|worker-runs`):

1. **The application's own presents**, where CE has that view: `CWrapDXGISwapChain::Present` when the game's create
   reached CE's factory wrapper, which is DX12's normal case. Exact. A window with worker runs beside it logs them too,
   which cross-checks (2) on DX12 (`modalRun`).
2. **Runs on the interposer's workers.** NvPresent hands each application frame to one of its workers, which presents
   all of that frame's outputs. A run is the presents between two thread changes; the factor is the most common run
   length, and a longer run counts as that many frames, because a worker can get two frames in a row. Trine 5 DX11, sessions `20261106_151156` and `171629`: **51574 presents in 25711 runs, 25639 of length 2,
   70 of 4 or 6, 2 of 1**, at 20 to 234 base fps, capped and uncapped, with gaps inside the pair of 500 us to 10 ms. No
   timing and no threshold: the cadence is whatever it is. 32 presents on one thread is a dedicated presenter, whose runs
   say nothing, and it logs `FG: The present interposer's output stream is on one thread`. Replaying those thread
   sequences through the class: 305 windows over nine game runs, all decided 2x, status never lost (log timestamps have
   1 ms resolution, which the rates can absorb and the runs do not use).
3. **The game's own submissions** (`IsApplicationSourcedInterposerPresent`), only for a dedicated presenter in DX11.
   Zero draws since the last present proves a generated frame. The other direction proves nothing: a game still drawing
   while the interposer presents counts every present as its own, so this can only undercount the factor. It was the
   only DX11 evidence until 2026-11-06. Submission windows keep their own counts. Direct application Presents take
   precedence when available, and a worker change by itself supplies no 1:1 verdict.

Why DX11 has no view (1) in Trine 5: `CWrapD3D11Device::QueryInterface(IDXGIDevice)` returns the real object, so
`GetAdapter` and `GetParent` reach the real factory and the game's create never touches CE's wrapper. Returning
`CWrapDXGIDevice` there would give the view, at the cost of changing the DXGI identity of every DX11 game; (2) does not
need it.

### From windows to status

- A window needs a second and 20 application frames, or 4 s and 5 frames. Fewer in 4 s closes it undecided.
- **An undecided window says nothing.** It used to clear the status: every window of a game below 20 fps did.
- `>= 1.5x` counts as generating, rounded and clamped to 4x.
- `GenerationLatch`: three decided windows in a row must say "not generating" before the status clears, one generating
  window starts that count over.
- A gap of a second in the output stream restarts the window, so an alt-tab publishes no rate.
- Once a window decided, `DetectPattern` cannot set the status any more. Before, it re-detected 40-100 ms after each
  window that disagreed. `ClearNvidiaSMState` starts the evidence, latch and cadence authority over together, so a new
  epoch without decisive windows can use the heuristic fallback again. The clear log reports `cadenceAuthorityReset=1`
  once when releasing that authority, even if Smooth Motion was already inactive. Virtual-clock regressions in
  `tests/test_present_interposer_evidence.cpp` cover recovery after both generating and forwarding windows.

### What the overlay shows

- **The output rate is what reaches the screen**, from the display stream, as for every generator. With vsync the
  interposer can submit more than the panel shows (Witcher 3: 288/s on 143 Hz). The interposer's rate stands in while
  there is no display stream.
- **The base rate is the measured application rate**, never above the output rate. Dividing output by the factor is
  wrong exactly when the driver generates past the panel.
- **The presentation series holds every output present**, as under every other generator: it is the fallback for the
  main frame rate and the graph when the display stream is not available. DX11 used to skip presents classified as
  generated, which halved both. `frameTimeSource=display-change` is unaffected either way.

### Session `20261106_171629`: how status was lost

Trine 5 DX11, 0.1.7434, 144 Hz: Smooth Motion ran 2x for all 22 s (present gaps 10000/3900 us at exactly 144/s), and
every window read `application=144.0 output=144.0 generating=0`. NvPresent64's `D3D11CreateDevice` was CE-patched, so its
device was a CE wrapper; `GetDevice` on the private chain returned it, and CE's overlay and NvPresent's own draws all ran
through `CWrapD3D11DeviceContext`, counting as application submissions on every present. The status flipped every second
between that window and `DetectPattern` for 4 s and then stayed off, once the pair gaps stopped reading 3:1
(`smCadence=0`). The deferred AF bootstrap also retried on that uncooperative wrapper every Present, writing 17 hook
warnings per Present: 87% of the log.

## FPS limiter
`ce::interposer_output_pacing` owns this. **Where CE sees the application's Present, it paces that**: CE's swapchain wrapper
is the input site (`PresentSite::kRuntimeInputPresent`) on the game thread, the outputs pass while it is live, and the
driver spaces them, the same model as FSR FG. Where CE only sees the outputs (Trine 5 DX11, Vulkan), they are the site
(`kInterposerOutputPresent`, 5): the first output of a frame waits for the group's slot on the exact rational grid, and the
frame's later outputs each get an even share of that group. A frame whose first output missed its slot passes whole.

| Trine 5 DX11, 2x | display stddev | jaggedness | p1 / p50 / p99 | flip lead | present-to-display |
| --- | --- | --- | --- | --- | --- |
| No limiter, 109/s (`180824`) | 78-250 us | 87-129 us | 8900 / 9100 / 9500 us | 4345 us (half an interval) | 21 ms |
| One wait per frame at 140, 0.1.7435 (`181807`) | **1315 us** | 1867 us | 3700 / 7100 / 8700 us | 770 us | 6.3 ms |
| One wait per frame at 120, 0.1.7435 (`181807`) | **1436 us** | 1030 us | 400 / 8300 / 9000 us | 3166 us | 4.4 ms |
| Every output its own slot at 140, status undetected, 0.1.7434 (`151156`) | **133-237 us** | 155-192 us | 6700 / 7100 / 7500 us | ~650 us | 1.5 ms |
| Smooth Motion off at 140 (`151156`) | 98-111 us | 103-115 us | 6800 / 7100 / 7400 us | - | 0.75 ms |

The waited presents themselves were exact in the second row (release to release 14286 us, stddev 13 us): CE's grid was
fine, the unwaited present beside it was not. Present calls within a pair arrived 0.2 to 3 ms apart. The duplicate window that
told the two apart was also a time threshold, so next-frame presents inside 2 ms escaped (window #3: 176.8 outputs/s at a
140 cap) and the waited present could move to the other output of a pair. No lock contention on the workers in any of these
sessions (`strict grid serialized` count 0), though `kInterposerOutputPresent` blocks on the cadence lock like any strict site.

Each worker keeps its own group position and armed output deadlines, using the multiplier's count and no clock-based
grouping. Interleaved workers preserve one another's group state. A detection flap cannot change the cadence
either: with no status yet each output owns a slot of the same output grid. Below the cap CE adds nothing, because a CE
wait there could only disturb the driver's flat schedule (first row).

Base-frame capture sync on this site keeps its target as the frame rate, with the later outputs spaced within that group.
The explicit Reflex base-frame sleep stays on the application route. Interposer output workers arm the later output
slots on the same local cadence grid. With Vulkan async present, Smooth Motion owns pacing at queue present, while
ordinary async rendering keeps it at acquire.

Diagnostics: `Apply: ACTIVE ... site=5` and `FPS Limiter: pacing the present interposer's outputs` for the output route;
`DXGI limiter: the application's Present into the present interposer's proxy` for the input route;
`FPS Limiter: present interposer output spacing (N outputs): spaced= waitMeanUs= late= lateMeanUs= lateMaxUs= unscheduled=`
every 600 outputs. `unscheduled` counts outputs left to the driver because the frame missed its slot; `late` counts
outputs of a capped frame that reached CE after their own slot. In `sensors.log`, `publishedInterval` is the screen and
`nvFlipSchedule avgDelayUs` is cumulative: difference `avg x applied` across lines to get a window's lead.

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
- `DX11: Creating transient RTV for SwapChain <ptr>` / `DX11: Releasing <n> retained swapchain RTV(s) — this Present
  is on a present interposer's private chain` — retention actually dropped. A `Creating retained RTV` line for a chain
  the create log called the interposer's is the regression.
- `FG: Present interposer output cadence window #N - application=... output=... generating=... multiplier=... decided=...
  stream=... runs=... modalRun=...` First five, every 60th, every change, for every API. `modalRun=1` with `stream=worker-runs`
  while the screen says otherwise means the driver stopped presenting a frame's outputs on one worker: read the present
  threads before anything else.
- `FG: NVIDIA Smooth Motion ACTIVE|inactive from the present interposer's output cadence` — the status itself.
- `[Overlay] FG rates diverged from the runtime's figures` under `type=3` — what the interposer submits is not what the
  screen shows, which is expected when it generates past the panel.
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
- Separating the two present streams by a gap threshold. The pairing is driver metering, not a contract:
  the same pairs ran 10000/3900 us and 6000/500 us in one session.
- Telling frames apart by the game's submissions alone. It looks structural, and a game that is still drawing while
  the workers present breaks it. It survives only as the last evidence.
- Hooking the proxy's Present through NvPresent64's vtable. Its RTTI has no swapchain class, so finding the table is a
  scan that the next driver can break, and the worker runs answer the same question without touching it.
- Pacing a frame's outputs with one wait and a time window for the rest: that was site 1 under frame generation, and it is
  what the limiter table above measures.
- Spacing the later outputs of a frame that missed its slot. It would put a CE wait on top of the driver's own schedule
  exactly where that schedule is flat.
- Adopting the render target bound at Present entry as the overlay target on an interposer's private chain.
- Deciding the interposer route inside a per-API branch. It is a property of who owns the chain, not of the API; the
  Witcher 3 crash below is what that mistake cost.

## CE's hook-discovery devices must never initialize the interposer
Witcher 3 DX12 + Smooth Motion, session `20261001_032227`, build 0.1.6868: `int 3` inside NvPresent64 on CE's hook
thread before the first frame. NvPresent64 statically links Microsoft Detours and, while a hardware device is being
created, attaches to `dxgi!CDXGIFactory::CreateSwapChainForHwnd`; it breaks into `int 3` when
`DetourTransactionCommit` fails. Detours allows one transaction per module at a time, so a second thread entering
the same init gets `ERROR_INVALID_OPERATION` (0x10DD, the value in `eax` at the breakpoint). The game's main thread
was inside `D3D12CreateDevice` (NVIDIA UMD init), while CE's thread was in the `DX11Hook::Init` temp probe creating a
**hardware** D3D11 device. The game's attach won: the dump holds its trampoline in NvPresent64's cache.

Why CE was in the DX11 path at all: NvPresent64 imports `d3d11.dll`, so the game's own D3D12 device creation mapped
it, and `D3D12CreateDevice` had not returned yet (`DX11 check #57 ... dllPresent=1 ... d3d12Created=0 => INSTALL`).

Fix: the DX11/D3D10 temp probe is WARP-only with no hardware fallback, the same rule as the DX12 temp-swapchain
bootstrap (`dx12-injection-bootstrap.md`). The harvested vtables live in d3d11/d3d10/dxgi, not the driver;
`DX11HookDiscoveryProbeTest` compares every `ID3D11Device`, `ID3D11DeviceContext`, `IDXGISwapChain` and
`ID3D10Device` slot between WARP and hardware on the test host. Signature in a log: `DX11: Temp D3D11
hook-discovery device is WARP`. Not yet hardware-validated in Witcher 3.

The same audit found the D3D10 sampler hook writing `ID3D10Device` slot 9, which is `Draw`;
`CreateSamplerState` is slot 86 (`hook/d3d11/d3d10_vtable_slots.h`).

## Open questions / stale-risk
- CE still installs DX11 hooks in this DX12 process, because a driver-loaded `d3d11.dll` counts as DX11 presence
  while the game's `D3D12CreateDevice` is still running. That is now harmless (WARP probe), but the classification
  itself is unchanged.
- Stale-risk **medium**: the classification is keyed on the `nvpresent` module name. A driver that renames or
  relocates the interposer, or a different vendor shipping the same topology, needs the token table extended.
- **The output-chain topology has not been run on hardware yet.** 0.1.6555/0.1.6556 validated the app-facing
  fallback (`20260914_105853`); 0.1.6557 moves the overlay to the output chain on the interposer's queue. Watch for
  `devRemoved=0x887A002B` on the first `Reinit SUBMIT`: if it returns, the queue was not the whole story and the
  next suspects are the backbuffer resource state NvPresent64 leaves before Present, and whether it tracks
  submissions on its own queue.
- **The limiter's output slots have not run with a detected status.** The table's evidence is every output on the grid with
  the status undetected; the same grid with the status detected is new code. DX12's input site and Vulkan's output site have
  no measurement at all. A first DX12 run should show near the first table row's flatness; if it does not, compare
  `nvFlipSchedule` lead against half an interval before anything else.
- Under the output route the game is only throttled by NvPresent's queue, so the frame ages there. The input route needs CE's
  view of the application's Present, which Trine 5 does not give (`Where the application's frames come from`).
- **The worker runs are a driver implementation detail**, measured on r621 in DX11 only. Stale-risk **high** on a driver
  update. A driver that presents each output on its own worker reads `modalRun=1`, not generating. Nothing else
  would catch that in DX11: compare with the display rate if it ever happens.
- **Vulkan topology is inferred, not observed.** `nv-vk64.json` declares `VK_LAYER_NV_present` as a driver implicit layer,
  and the loader lists those before registry layers, so it should sit above CE's layer and CE should see the outputs. The
  next Vulkan Smooth Motion run answers it: `stream=worker-runs` is that case; the one-thread line means CE sits above it
  or NvPresent presents from one thread, and only the 2026-07-29 heuristics are left.
- A DX12 run's `modalRun` beside `stream=application-presents` shows whether the worker runs hold there too.
- DX11 Smooth Motion **does** have the same proxy topology — confirmed on hardware, Witcher 3, session
  `20260919_154534`: NvPresent64 created two private output chains on the real factory and CE's only Present view
  was those chains, never the application's. CE's overlay device there (`0000022DD76731C0`) is NvPresent64's own
  D3D11 device, reached through `GetDevice` on the private chain, not the game's (`0000022DB68BF0D0`).
  **Fix not yet hardware-validated.**
- Still open in DX11: the overlay device/context are cached from the FIRST chain CE draws on, while Smooth Motion
  creates TWO private chains on two different devices. A Present on the second chain would build its RTV with the
  first chain's device. Not observed (only chain #1 ever presented that session), not ruled out.
- CE's factory wrapper is what sees the application-facing create. A game that reaches the real DXGI factory without
  it would leave CE with no app-facing view at all under an interposer; not observed, but not ruled out.
