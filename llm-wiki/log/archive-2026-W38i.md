# llm-wiki Log — archive 2026-W38i

### 2026-09-16 - Gothic II runs; the alt-tab freeze is the game's own error dialog

Session `20260916_014133` is the first clean Gothic II run since the DirectDraw work started. The
native D3D7 sidecar primed, the render loop armed within a second (`monitoredTid=812`), the route
held `native-d3d7` with `nativeFail=0` across 4,040 flips, and **the presentation re-entry guard
logged nothing at all** - the cycle that killed the previous three sessions did not form.

The whole diagnostic chain worked for the first time: the watchdog saw a dialog at five seconds and
*suppressed* the dump because the render heartbeat was fresh, then dumped at 26.5 s once the
heartbeat went stale, and the dump carried 35 WoW64 thread stacks. Everything added over the previous
commits earned its place in one run.

The alt-tab freeze is not CE's. At `01:42:36.820` four consecutive
`IDirectDraw7::CreateSurface` calls returned `0x8876024E` (`DDERR_UNSUPPORTEDMODE`) with
`surface=NULL`, after ~280 successful ones - the display mode was gone. Gothic/SystemPack then put up
its own modal `Error-Message` box, and the frozen render thread is parked in that dialog's message
pump (`NtdllDialogWndProc_W` -> `NtdllDispatchMessage_W` -> `PeekMessageW`, drawing through
`gdi32full!ExtTextOutW`). No CE frame is in that chain; the `capture_hook_x86` symbols on that stack
sit above the live frame and are residue from an earlier logging call. On a fullscreen DirectDraw
title that dialog is invisible behind the game window, which is what "froze" looks like.

Not established either way: CE holds DirectDraw references of its own in that process - the bootstrap
object, and the sidecar's device, `IDirectDraw7` and font surface - and whether those contribute to
the mode restore failing has not been tested. The cheap experiment is an alt-tab with CE not
injected.

Worth a look when the DirectDraw CPU composite next gets attention: `compositeMaxUs=55856`,
`lockMaxUs=27109`, `writeMaxUs=44395` in this session - single hitches of tens of milliseconds on the
render thread, on the loading-screen path rather than the native route.

### 2026-09-16 - Handing the cycle another function to call did not end it

Session `20260916_013230`, the first run with the cycle break from `23fc0101`. The guard fired on the
very first flip and kept firing: `occurrence=1` through `32768` inside two milliseconds, every line
reporting that it had found a DirectDraw-owned entry point and called it. The game died five seconds
later with exit code `0xC0000005` and an empty `crash.log`.

So the escape was wrong, and worse than wrong: removing the 4K composite from each level turned a
recursion that took thirteen seconds into one that exhausted the stack in two milliseconds. The
recorded entry point was not the problem in the obvious way - `Recorded 3 of 3` for vtable
`7a1bd040`, snapshotted on the bootstrap thread before the game's surfaces existed, and
`ddraw!DD_Surface_Flip` disassembles with no dispatch back through a surface vtable. Something else
re-enters CE there, and the fact of the recursion alone cannot say what.

A nested presentation now returns without calling anything. That is the only response that cannot
recurse by construction; the cost is one dropped presentation. `NoteDirectDrawPresentCycle` reports
the module that re-entered CE, CE's saved original, and the entry point recorded before CE patched
the slot - three names that separate a co-resident overlay from DirectDraw itself from CE calling
back into its own detour, which is what the next occurrence has to settle.

**Method note, twice over now on this bug.** The previous entry's reading of
`20260916_005504`/`011148` was corrected by a dump; this one was corrected by the very next run. Both
times the failure was reasoning past the evidence to a mechanism that fit. The guard shipped here is
deliberately the version that needs no theory of the caller to be safe.

### 2026-09-16 - Gothic II's freeze was CE and Steam calling each other's Flip detour forever

Session `20260916_011148`, diagnosed from a dump taken of the live hung process. The repeating cycle
runs down eight megabytes of the render thread's stack - 322 copies in a single 48 KB window:

    capture_hook_x86!DetourDDSurface7Flip+0xde
    gameoverlayrenderer!OverlayHookD3D3+0x8dbc
    gameoverlayrenderer!VulkanSteamOverlayProcessCapturedFrame+0x83330
      -> capture_hook_x86!DetourDDSurface7Flip+0xde ...

`+0xde` is the return address of exactly one instruction, `call eax` at `+0xdc`, where
`eax = ddraw_hook_oDDSurface7Flip`. CE calling its own saved original lands in Steam's overlay, and
Steam's call to *its* original comes straight back into CE's vtable detour. Both overlays hooked
`IDirectDrawSurface7` slot 11, interleaved, and each ended up holding the other's detour.
`VTableHook::Create` has no equivalent of the `IsAlreadyHooked` / "preserving external entry"
protection `InlineHook` already carries.

**This invalidates the previous session's reading.** `NoteDirectDrawPresentationAttempt` sits at
`+0xed`, after the call that never returns, so `renderLoopObserved=0` was never a watchdog defect -
no Flip ever returned. And the "3,600 presentations at ~270/s over thirteen seconds" was 3,600
*recursion levels*, each running a full 4K composite: at the measured ~3.7 ms per composite that is
13.3 s, which is exactly the window. Nothing reached the screen because the real flip was never
called. The `DDERR_SURFACELOST`/E_FAIL locks at the end are the stack running out, not a cause.

The fix keeps an escape that provably belongs to DirectDraw.
`RecordDirectDrawPresentEntryPoints` snapshots the Flip/Blt/BltFast slots before CE patches them and
keeps each one only if it lies inside `ddraw.dll`; all nine presentation detours now refuse to
re-enter themselves on a thread and break out through that pointer instead of their saved original,
so the cycle ends at its first bounce with the flip still performed. Without a recorded entry point
the nested call returns without presenting - one dropped frame is bounded, a hang is not. A nested
blit that is not classified as a presentation is ordinary CE capture work and still passes through.

**Second defect, which is why there was no dump:** the overflow did reach CE. The frozen thread sat
in `KiUserExceptionDispatcher -> RtlDispatchException -> RtlpCallVectoredHandlers ->
CrashHandlerExceptionFilter -> TraceCrash -> pthread_mutex_lock -> WaitForSingleObject`, with
`crash.log` still empty. Windows re-enters a vectored handler on the same thread, and `TraceCrash`
took a non-recursive `std::mutex`, so the handler waited on a lock its own thread held.
`ExceptionSafeLock` now takes `g_TraceCrashMutex` and `g_DumpDirMutex` only when the calling thread
does not already own them.

Also confirmed working from this session: the presentation-window dialog fix (`dialogTid=0`, was
28632), and the WoW64 stack collector, which is the only reason any of this was recoverable - the
user's own Task Manager dump of the same frozen process had no 32-bit stacks at all.

### 2026-09-16 - Gothic II froze with every present failing, and the watchdog dumped the wrong 5 seconds

Session `20260916_005504`. The game did not freeze at start: it ran about 3,600 presentations at
~270/s for fifteen seconds, then its render thread produced its last log line at 00:55:27.8 and never
another, with GPU load dropping 71% -> 1%. Four findings, three of them CE's.

**Every application `Flip` failed for the whole session.** `NotePresentationComplete()` runs only on
`SUCCEEDED(hr)`, and it never ran once: `renderLoopObserved=0` across all 91 s and no presentation-mix
line, while the composite counted 3,600 presentations. Both healthy sessions armed the render loop
within a second. CE logged nothing about it, because a failing Flip was invisible to every counter CE
keeps - the overlay composite still ran, the mix counters still moved, and the application kept
looping. `NoteDirectDrawPresentationAttempt` now reports a rejected presentation with its operation
and HRESULT, once per failure run plus a recovery line.

**CE retried a failed surface lock without `DDLOCK_NOSYSLOCK`.** One second before the wedge the
overlay composite's `Lock` started returning E_FAIL, and the fallback dropped the flag - which makes a
`DDLOCK_WAIT` lock take the Win16 lock, on the application's render thread, inside its own present,
in a process that also hosts Steam's overlay and a message pump. Two capture locks had the same
"progressively compatible" ladder. The flag is now kept on every attempt; only the read-only hint is
negotiable, and a lock CE cannot take is a frame CE does not composite - which both callers already
handled. `DDrawLockFlagsTest` reads the sources so the fallback cannot return as a compatibility fix.

**The watchdog wrote a 29 MB "blocking dialog" dump for the game's own render window.** `hwnd=00320ae0`
is the window CE composites into - `Overlay target changed ... newHwnd=00320ae0` names it - and Gothic
II registers it with class `#32770`. The dump landed at 00:55:17, while the game was running at 270
presentations a second. CE always knows its presentation window, so that window is now excluded from
the dialog scan outright.

**CE could then never dump the actual freeze.** `renderLoopObserved=0` (the first finding) made
`ShouldAssertRenderThreadFreeze` refuse for the rest of the session; the watchdog logged
"no authoritative present is in flight" every ten seconds and the one dump it did take was ten seconds
early. An application that called Flip and got an answer has a live render thread whatever the runtime
answered, so the watchdog now arms on a returned presentation rather than an accepted one.

The user's own Task Manager dump of the frozen process carries no 32-bit stacks either - the same
WoW64 gap `d1eb4998` closed for CE's helper, which Task Manager's x64 writer still has. CE's dump is
the only one that can show this freeze, which is why the two watchdog defects mattered more than they
look. Whether the wedge itself is the Win16 lock is not yet proven; the next occurrence should say so
directly.

### 2026-09-16 - A Direct3D 7 state block restored a texture the game had already destroyed

Gothic II/SystemPack session `20260916_000027` crashed about seven seconds in, during the intro
videos: `0xC0000005` reading address 0 at `D3DIM700.dll+0x9982`, which symbols resolve to
`DIRECT3DDEVICEI::SetTextureInternal+0x12`. The instruction is `mov eax,[eax]` after
`mov eax,[pSurface+4]`, so the surface interface handed to `IDirect3DDevice7::SetTexture` had a NULL
`lpLcl` - a DirectDraw surface that has already been destroyed.

The caller is not the application. `SetTextureInternal` is reachable only through device vtable slot
82; the public `SetTexture` wrapper (slot 35) dispatches into it and keeps EBX as its 0/1 multithread
flag, while the crash recorded `EBX=0x0a4d9d40`. The other route to that slot is the pointer the
device caches at `+0x3280` for its table-driven state replay, refreshed in `EndScene`. CE's overlay
sidecar is the only state-block user in the process.

A Direct3D 7 `D3DSBT_ALL` block records each stage's texture as a raw `IDirectDrawSurface7*` and takes
no reference. Nothing in such a process ever re-binds an old texture, so an application releasing a
still-bound surface is legal - the device keeps its internal texture object alive and the surface
interface is never touched again. CE's `ApplyStateBlock` is what re-binds it, and that is what turned
a legal application pattern into an access violation. The earlier attributions of this title's
crashes to "the native backend running at all" (339eccf0) named the right subsystem for the wrong
reason.

The fix supplies the missing lifetime guarantee rather than removing the state block: CE hooks
`IDirect3DDevice7::SetTexture` on the same vtable it already hooks for forced filtering, and owns a
reference to every binding for as long as the device holds it - at most one surface per stage,
released the moment the application binds something else. The sidecar refuses to prime on a device
whose bindings CE has not owned since `IDirect3D7::CreateDevice`, falling back to the CPU composite,
and stage 0 is restored explicitly from that shadow after the block so the sidecar no longer depends
on the block carrying textures at all. `LegacyD3DTextureBindingsTest` covers the ownership rules;
`LegacyD3D7VTableAbiTest` pins slot 35.

Validated on hardware the same night, session `20260916_004304`: the sidecar primed, the route
stayed `native-d3d7` for the whole run with `nativeFail=0` over 5,994 flips, pacing held 144.0 fps at
11 us frame-time standard deviation, an included screenshot completed mid-run, and the process exited
cleanly with no `crash.log` and no dump. Verifying that also exposed a gap in the gate itself: it
proved the device had been seen at `CreateDevice` but not that the `SetTexture` interception had
actually installed, so a vtable slot another overlay owned would have left an empty shadow looking
trustworthy. The gate now requires both, and a failed install is logged instead of silent.

The WoW64 dump work below is still unexercised - nothing crashed, so no dump was written.

### 2026-09-16 - WoW64 crash dumps now contain the 32-bit stacks

Diagnosing the above took opcode archaeology because the dump had no 32-bit stack at all, and that is
the second time: commit 339eccf0 recorded the same gap as open. CE's external dump helper is x64, and
`MiniDumpWriteDump` records a thread's stack from the CONTEXT it can see - for a WoW64 thread written
by a 64-bit dumper, the x64 side, which holds only the syscall thunk. The dump loads, resolves
symbols and prints registers, and cannot produce a single caller.

`captureengine/dump_helper_wow64_stacks.{h,cpp}` supplies each thread's committed 32-bit stack
through dbghelp's `MemoryCallback`. The stack pointers are read inside dbghelp's own thread callback,
where the target is already frozen for the dump, so nothing is suspended twice and no context can be
torn; a thread sweep covers a dbghelp that would ask for memory first. The walk follows adjacent
committed regions of the same reservation and is capped at 1 MiB per thread, 64 MiB and 512 ranges in
total, so a process with hundreds of threads cannot turn a crash dump into a full-memory dump. A
64-bit target is untouched. `crash.log` now records how many ranges were added, and
`WriteSupplementalCrashDump`'s smaller-dump retries re-emit the same ranges instead of the first
attempt consuming them. `Wow64StackRangePolicyTest` covers the arithmetic and the caps.
