# llm-wiki Log Archive 2026-W38d

### 2026-09-14 - Gothic II start crash: the composite ran with the native backend still loaded

Session `20260914_182411` (0.1.6584) died five seconds in: `0xC0000005` reading address 0, in
`gameoverlayrenderer.dll+0xB8EA0` - Steam's inlined SSE `memcpy`, `movdqa xmm0,[esi]` with ESI null, copying about
1700 bytes from nothing - on the game's own DirectDraw render thread. The external helper captured the dump after
the fact, so it carries no exception context and no usable stack; the attribution below comes from the log, which
is unambiguous about what CE was doing for the half second before it.

`[Overlay] Initializing D3D7 backend` appears exactly once in the whole session. `Initializing DX9 backend` never
appears. Every `RenderOverlay#N` line reports the same `renderer=0c032610`. And from 18:24:21.781 the route was
compositing: `DX9: Registered internal helper device`, `Overlay composite region resized`, `Overlay composited
into the presented surface` counts 1-4.

So the native backend drew three flips, the application stopped rendering into the surface being presented, and
`TryDrawNativeLegacyD3DOverlay` correctly declined - but the composite path that took over called
`OverlayAdapter::InitDX9`, **which returns true without doing anything when an adapter is already initialized**.
The backend stayed bound to the application's Direct3D 7 device. Every composite frame after that issued
`BeginScene`, sampler and render-state changes, a draw and `EndScene` on the game's own device, at a point the
game never asked for, *between* CE's own Lock and Unlock of the presented surface, and outside
`LegacyD3DInternalScope` - so `ce::legacy_d3d_sampler_state` recorded CE's sampler states as the application's and
`ReconcileAfterExternalStateChange` re-applied forced filtering after every one of CE's own state restores. Then
it read back a helper backbuffer the overlay had never been drawn into and wrote that over the frame.

The fix is a rule, not a patch: **the backend the adapter holds must match the route actually executing, and
nothing renders while they disagree.** `ddraw_hook_overlay_route.cpp` derives the required route from the
presentation (`ce::ddraw_present_policy::SelectOverlayRoute`: only a flip the device actually rendered can use the
native route - a blit publishes an offscreen image and a direct scanout write is not a device operation at all),
switches the adapter to it before anything renders, and **confirms the backend that is actually loaded afterwards**
rather than trusting the Init return. `BackendCanRenderRoute` is the invariant in one expression and is unit
tested in both directions. Route switches are counted and bounded at eight, after which the route latches on the
composite, which works for every presentation shape; the periodic `DDraw: Presentation mix` line now carries
`route=`, `routeSwitches=` and `routeLatched=`.

Two pieces of hardening on the native backend itself. It no longer draws when `BeginScene` fails - a device
already inside a scene is mid-way through the application's own geometry, and at a flip that never happens, so a
failure means something unusual and the frame is skipped. And a failed `ApplyStateBlock` now disables the backend
outright: the application's device is carrying the overlay's blend and stage setup at that point and there is no
way to put it back, so the only correct action is to stop drawing.

Built and verified at 0.1.6586. **Hardware run pending.** The next run should show `DDraw: Overlay route -> ...`
on every transition and `route=` in the mix line; a session that ends up on `route=d3d9ex-composite` with
`routeSwitches=1` is the expected shape if Gothic stops rendering into its flip target early.

### 2026-09-14 - Gothic II loading screens, and the DX7 overlay that costs the GPU nothing

Session `20260914_180020` (0.1.6578) proved the flip-path fix: `kind=1`, the composite target is the attached back
buffer, a 512x320 region instead of the 3840x2160 frame, no `PresentEx` fallback, and 600 composites in 5.38 s -
112 per second where the full-surface round trip had managed about 17. The overlay still disappeared on loading
screens, and that was a second bug in the same policy.

**A flip chain's front buffer is still the screen.** `ClassifyBlit` suppressed every blit whose destination
belonged to a flip chain, on the reasoning that Flip publishes those images. That is true of a *back* buffer and
false of the primary: a blit onto the surface being scanned out is visible the moment it completes. Gothic II
draws its loading screens exactly that way - the 3D scene is not running, so nothing flips and the progress
display is blitted straight onto the primary - and CE composited nothing for the whole load. The predicate is now
`destIsBackBuffer` (DDSCAPS_BACKBUFFER without DDSCAPS_PRIMARYSURFACE) for "published later", with
`destOwnsFlipChain` kept only to decide *where*: such a blit's source can be a static image the application blits
again unchanged, so the overlay goes into the visible surface after the blit rather than being stamped into that
source.

A presentation-mix counter set now logs every ten seconds (`DDraw: Presentation mix ... flips= blitPresents=
directScanoutBlits= ignoredBlits= scanoutUnlocks= composites= skippedNoPublishedImage= skippedOutsideOverlay=`).
DirectDraw has no single present entry point, so a route that composites nothing is otherwise indistinguishable
from one that is never called - which is precisely what this looked like in the log.

**The overlay now draws with the game's own Direct3D 7 device.** The D3D9Ex composite, even region-scoped, ends in
`GetRenderTargetData`, which blocks the render thread until the GPU has caught up - a hard CPU/GPU serialization
point on the game's own present path, once per present, plus two video-memory locks. A DX6/DX7 title already owns
a device that draws transformed, alpha-blended, textured triangles, which is exactly what the overlay's draw list
is. `CustomOverlay::D3D7Backend` converts the shared vertices to `D3DTLVERTEX`, uploads the font atlas once as a
managed ARGB8888 texture surface, and issues the same handful of `DrawIndexedPrimitive` calls the DX9 backend
issues - no readback, no second device, no staging surfaces, nothing leaving the GPU. `TryDrawNativeLegacyD3DOverlay`
takes it only when the device's current render target *is* the surface this presentation publishes; anything else
keeps the D3D9Ex composite, which works on any surface. State is saved and restored with a `D3DSBT_ALL` state
block, and the overlay suppresses itself rather than running without one.

Three things had to come with it. The legacy `d3d.h`/`d3dtypes.h` headers redefine enumerators `d3d9.h` also
defines, so they exist in exactly one translation unit and the backend's interface is `void*`; the few device
methods the hook itself calls go by vtable index, pinned by `LegacyD3D7VTableAbiTest` in the only test that sees
the real declaration. CE's own state calls bypass the forced-filtering interception through
`LegacyD3DInternalScope` - that layer caches what it believes the *application* asked for, and the overlay's
sampler states are not the application's. And the D3D9Ex helper device is now created only when the composite
route actually needs it, with the adapter LUID the host's telemetry wants published from a device-less
`Direct3DCreate9Ex` instead.

Hot-path work that came out of the same pass: `TrackLegacyD3D7Device` runs from `SetTextureStageState` - once per
material - and took a mutex on every call, so the unchanged case is now a relaxed atomic compare;
`ActivateDirectDrawSurface` runs from every hooked Flip, Blt, BltFast and Unlock and cost a `QueryInterface`, a
lock and two hash lookups each time, now memoized per thread against an association generation; and the native
path's render-target identity check compares pointers before falling back to COM identity.

Measured, for the record: `cpu_prerender_limit` defaults to -1 and neither branch of `ApplyPrerenderLimitDDraw`
runs at that value, so CE adds no flip-status spin of its own. Built and verified at 0.1.6580. **Hardware run
pending** - no Gothic II session has exercised the native D3D7 backend yet, and the loading screens need a
re-check with the presentation-mix counters in the log.

### 2026-09-14 - DirectDraw overlay flicker: the composite was on the wrong side of the present

Gothic II with the SystemPack (DirectDraw7 + Direct3D7, 4K, session `20260914_173658`) drew the overlay and then
lost it again, frame after frame. The log shows what the route actually did: `DDraw: Overlay writeback to primary
surface completed` on every present, and `DDraw: Overlay helper PresentEx hr=0x08760878` - `S_PRESENT_OCCLUDED` -
on every one of them too.

Both lines are the bug. `HandleCapture` ran **after** the original `Flip`/`Blt` returned and composited into the
surface that was already on screen: read the primary, draw the overlay over it, write the whole thing back. On a
flip chain that write lands in a buffer the display is already scanning out, and the next flip replaces that
buffer with one the overlay never touched. The overlay was therefore present for part of a frame, missing for the
rest, and completely absent whenever the write finished after the following flip - flicker at the frame rate, by
construction. The helper `PresentEx` was a second, redundant presentation route that is occluded for as long as
the application holds the display, so it never contributed anything either.

The cost made it worse. Every composite moved the whole 3840x2160 surface up and back down across the CPU, about
66 MB per present; the session's composite counter advanced 112 times in 6.4 s, roughly 17 presents per second,
and the window in which the frame was on screen without the overlay was about 10-25 ms wide.

`hook/common/ddraw_present_policy.h` now names what each hooked call publishes. `Flip` publishes the flip chain's
back buffer (or the caller's explicit target), a full-surface blit onto a **single-buffered** scanout surface
publishes its source, and an `Unlock` of the primary is already visible. The overlay goes into the image the
present is about to publish, before the call reaches the runtime; when that image cannot be resolved CE composites
nothing rather than falling back to the visible surface, because that fallback *is* the race. Blits onto a flip
chain are no longer presentations at all - `Flip` owns those images - and partial blits are 2D updates that only
restore the overlay when they intersect it, instead of re-compositing the whole overlay dozens of times a frame
and feeding each one to the recorder.

The transfer is now the overlay's own bounding rectangle, aligned to a 64-pixel grid: `OverlayAdapter::
GetLastRenderedBounds` derives it from the geometry the renderer last built, and the staging surfaces are sized to
it instead of to the frame (over 20x less traffic at 4K). The rectangle is known only from geometry that already
exists, so the previous frame's rectangle stages the pixels and the frame that grows past it re-stages the union
and re-submits the same geometry through `OverlayAdapter::ResubmitLastFrame` - without that the overlay would be
clipped for exactly one frame every time it grows. The helper `PresentEx` survives only as the fallback for when
the in-frame composite fails outright.

Two smaller things fell out. The 100 ms "last presented source surface" tick heuristic is gone - `Flip` hands us
its target and `Blt` hands us its source, so there was nothing left to guess. And `NotePresentationComplete`
refuses to count a frame while a composite is in flight: the composite locks and unlocks DirectDraw surfaces
through CE's own hooks, so a nested `Unlock` of the scanout surface reaches the frame accounting and would both
corrupt the frame-time series and let the FPS limiter sleep inside the composite.

`DDrawPresentPolicyTest` covers the classification and the region arithmetic, including the two cases that were
previously wrong by construction: a flip composites into the flip target and never into the visible surface, and a
partial blit is not a present. Built and verified at 0.1.6577. **Hardware run pending** - no Gothic II session has
exercised this yet.

### 2026-09-14 - Gothic II/SystemPack startup: old-linker IATs and unrelocated pristine code

All three Gothic2.exe failures in sessions `20260914_151113` and `20260914_151846` were deterministic
`0xC0000005` reads in `msvcrt!strcmp+0x6c`, during `InitializeKernel32Hooks` immediately after CE patched the
main executable's `LoadLibraryA` import. The crash registers were identical: CE's literal `LoadLibraryA` at
`0x63A7551C` was compared with invalid address `0xE8421F72`.

The dump's live PE metadata resolves the equation. SystemPack's `Shw32.dll` was based at `0x71D40000`; its
Kernel32 import descriptor has `OriginalFirstThunk=0`, and the first `FirstThunk` value was the loader-resolved
`LoadLibraryA` pointer `0x766E1F70`. The old IAT walker treated `FirstThunk` as if it still held
`IMAGE_IMPORT_BY_NAME` RVAs, added the module base and the two-byte hint offset, and manufactured the exact fault
address: `0x71D40000 + 0x766E1F70 + 2 = 0xE8421F72` (32-bit wrap).

`iat_import_table.h` now keeps named INTs and name-less resolved IATs as distinct formats. A name-less entry is
matched only by the exact resolved export address, or by CE's own tracked hook record on a repeated pass; an
unidentified foreign replacement is preserved. All import directory, descriptor, thunk and string accesses are
bounded to `SizeOfImage` and checked for readable memory, with rate-limited malformed-table diagnostics.

`IATHookImportTableTest` reconstructs the exact name-less layout plus normal-name, malformed-RVA, tracked-hook,
foreign-owner and overflow directions. The first hardware re-check (`20260914_154220`, 0.1.6566) proved this fix:
the name-less patch completed and injection advanced through the rest of kernel32/GetProcAddress initialization.
It also exposed a second independent generic bug in the guarded DXGI bootstrap.

Steam owned `dxgi!CreateDXGIFactory1` with a five-byte entry jump, so CE built a clean bypass from the DLL's disk
bytes. The x86 disk prolog contains `A1 C0 86 0D 10` (absolute address `0x100D86C0`, based at the preferred
`0x10000000`), while the loaded image at `0x64210000` correctly contains `A1 C0 86 2E 64` (`0x642E86C0`). The
resume verifier mistook that legitimate relocation difference for more foreign patching, extended from +5 to +13,
then copied and executed the raw absolute operand. The crash was immediate and exact: EIP `0x0BA00008` in CE's
bypass trampoline, reading unmapped `0x100D86C0`. `llvm-readobj` confirms a `HIGHLOW` relocation at operand RVA
`0x38549` in the installed SysWOW64 `dxgi.dll`.

`inline_hook_pristine_image.h` now parses bounded x86/x64 PE file layouts and applies overlapping `HIGHLOW`/`DIR64`
base relocations to pristine bytes for the module's actual load address. A moved image with missing, malformed or
unsupported overlapping relocation data is refused rather than made executable. Both deep and bypass trampolines use
the rebased bytes for live comparison, decoding and copying, and log the applied relocation count. The focused
functional tests reconstruct the exact Gothic/DXGI bytes, partial-span application, x64 `DIR64`, missing-directory
and malformed-block directions; a source-policy assertion holds the guarded bypass path to the rebased reader.
Hardware re-check pending.

### 2026-09-14 - The 183 MB Portal RTX exit dump: one termination, two verdicts

Session `20260914_130052`, `crash_external_fatal_exit_NtTerminateProcess_00000001_4bae8a0c.dmp`, 183 MB, for a
clean quit. `hook_debug.log` holds both decisions one millisecond apart:

```
13:02:26.900 FatalExitDump: Skipping pre-termination dump - ... (source=TerminateProcess code=0x00000001
             caller=00007FF7A88B0D6B module=...\NvRemixBridge.exe+0x10D6B fgRuntimeActiveOrRecent=1)
13:02:26.900 FatalExitDump: Capturing pre-termination dump ... (source=NtTerminateProcess code=0x00000001
             exceptionAddr=00007FF9C0830159 crashLike=0 fgRuntimeActiveOrRecent=1 origin=loaded-module)
```

The logged stack of the second decision names the requester outright: `stack[2]=KERNELBASE.dll+0x100159`,
`stack[3]=NvRemixBridge.exe+0x10D6B`. The 2026-09-02 primary-module rule worked exactly as designed at the
`TerminateProcess` layer and then lost, at the `NtTerminateProcess` layer, the one fact it depends on. A single
termination request is observed by every hook it passes through on one thread, and below the outermost layer the
immediate caller is always a Windows module — so `ResolveTerminationOrigin(callerAddress)` reported the plumbing,
not the requester, and the suppression path (which deliberately does not set `g_PreTerminationDumpAttempted`, so it
cannot burn the one-dump budget) gave the inner layer a second, worse-informed chance.

Fix (0.1.6565): frames, not the immediate caller. `ce::crash_dump_policy::ResolveTerminationOriginFromFrames`
walks innermost-first, skipping CE's own hook frames and the forwarding layers
(`kTerminationPlumbingModuleNames`: ntdll, kernel32, KERNELBASE, ucrtbase, msvcrt, vcruntime140), and takes the
first frame that is neither. `ResolveTerminationOrigin` only pays for that walk when the immediate caller is one of
those layers; a caller it can attribute directly is still answered without a stack walk. Classification stays a
pure range check against bounds cached by `CacheTerminationOriginModuleBounds()` at hook-install time — now the
executable, CE's own image and each plumbing module — because `NtTerminateProcess` is also reached from
`RtlExitUserProcess` with the loader lock already held by the terminating thread.

Deliberately narrow: the plumbing list is module names, not "anything under the Windows directory". NVIDIA's FG
runtimes load from the DriverStore under `C:\Windows`, and an FG runtime killing the process while tearing down is
the entire reason the active-FG fallback exists. Everything unprovable still dumps — an unresolvable frame stops
the walk at `kUnknown`, a stack of nothing but forwarding layers resolves to `kUnknown`, and `kUnknown` never
suppresses. Crash-like exit codes never consult the origin at all.

Both log lines now carry `requester=`/`requesterModule=` next to the immediate caller, so a future false positive
of this shape is one line to diagnose instead of a stack dump to read.

Tests: `CrashDumpPolicyTest.LayeredTerminationRequestIsAttributedToItsRequester` encodes the exact frame layout
from this session; `LayeredTerminationRequestFromALoadedModuleStillDumps`,
`UnattributableTerminationRequestResolvesToUnknown` and `OnlyTerminationForwardersCountAsPlumbing` cover the
fail-open directions. `CrashHandlerSourceTest.LayeredTerminationRequestIsAttributedByWalkingTheStack` holds the
resolver to the walk, and `TerminationOriginIsCachedAtInstallAndResolvedWithoutTheLoader` now covers the
classifier as well as the resolver for the no-loader invariant.

**Hardware run pending**: a Portal RTX quit should log the suppression line with
`requesterModule=...NvRemixBridge.exe` and produce **no** `crash_external_fatal_exit_NtTerminateProcess_*.dmp`.
