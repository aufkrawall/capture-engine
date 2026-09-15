# llm-wiki Log

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

### 2026-09-15 - DirectDraw overrides now own the presentation operation that actually runs

Gothic II/SystemPack session `20260915_132705` proved that profile resolution was not the problem:
`inject.log` published `vsync=fifo`, `af=16x`, and `cpuPrerender=1.00`. The game then issued 1,505 accepted
full-surface Blt presents and zero Flips in the ten-second sample. Both overrides lived only in the Surface7 Flip
detour, so they were complete no-ops on this borderless route. The sampler hooks were installed, but a safe sampler
that needs no physical transition produced no AF-specific evidence.

One presentation owner now covers Flip/Blt/BltFast across legacy, Surface4, and Surface7 detours. FIFO Flip rewrites
the actual flip flags; FIFO full-surface Blt/BltFast performs overlay/capture composition first, then waits on the
owning DirectDraw object's vertical blank immediately before the real publication call and uses blocking submission
flags. The WaitForVerticalBlank hook consumes an application's successful synchronous `BLOCKBEGIN` wait on the same
thread so CE never waits a second refresh; event and end-of-blank waits are not reusable publication boundaries. Its
fixed registry gives each vtable an indexed detour, preserving the correct
predecessor if a foreign overlay later replaces or clones the object's vtable. Ordinary drawing blits stay outside
the path. Logs expose the operation, route, flag rewrite, vertical-blank failures/timing, and queue wait timing.

The CPU queue is operation-typed and reference-safe: Flip completion uses Surface slot 18 (`GetFlipStatus`), while
Blt completion uses slot 13 (`GetBltStatus`); successful presents enter a fixed six-slot AddRef-owned ring and primary
or configuration changes release it. The removed helper had called slot 13 as a Flip method, retained raw surface
pointers, and implemented forbidden fractional limits with a synthetic sleep. DX6-8 device registration now logs
the resolved AF policy and maximum anisotropy, while per-stage decisions explain whether AF was allowed or preserved
for disabled mip filtering, point filtering, special addressing, or unsupported device caps.

The legacy sampler audit also checked the real D3D6/7 and D3D8 SDK layouts instead of trusting local numeric
declarations. ABI tests now cover every used device/vtable slot, all tracked texture-stage IDs, D3D7's distinct
MAG/MIN/MIP enum families, and D3D8 filter values. `mip_mapping=nearest|bilinear|trilinear` maps to point/point/point,
linear/linear/point, and linear/linear/linear respectively. It deliberately acts only when the application already
uses a mip chain; safe mode also preserves non-material addressing. A rate-limited per-stage mip decision reports
`allow`, `mip-filter-disabled`, or `non-material-address`, and shared-config logging now publishes both the sampler
policy and mip mode so a resolved profile cannot remain ambiguous.

Native DirectDraw Blt pacing remains fixed-refresh and zero-copy. It is not called VRR promotion: borderless VRR
requires a tearing-capable DXGI flip-model swapchain, which in turn requires a real DDraw/D3D7 translation backend.
A second-swapchain final-frame copy was rejected because it adds bandwidth, latency, synchronization, and another
overlay/capture ownership boundary on every frame.

### 2026-09-15 - Ungated compiler diagnostics closed; three latent defects and two dead subsystems removed

An audit of the whole tree found the project's quality gates were tight where they looked and absent where they
did not. `clang-tidy` ran with `-extra-arg=-w`, which switched every compiler warning off for the lint pass; with
no `-Werror` on the build either, `-Wall`/`-Wextra`/`-Wshadow`/`-Wformat=2` were completely ungated. The baseline
truthfully reported "0 warnings" over 756 translation units while 231 unique first-party warning sites sat unread
in the build log. `clang-analyzer-*` was not enabled at all.

Dropping the flag routes compiler diagnostics into the existing ratchet as `clang-diagnostic-<name>` with no new
infrastructure - `analyze_warning_output` already buckets on the trailing `[check]` token. Baseline recorded at
2224 and worked down to 1900 in the same pass. `clang-analyzer-core.*`/`cplusplus.*` and `misc-use-after-move`
are enabled too; some of their findings are cross-translation-unit false positives (a null-guard passed as a bool
into a policy header is invisible to the analyzer) and are carried in the baseline rather than suppressed inline.

Three defects these checks had been hiding, all fixed:

- `layer_overlay.cpp` dereferenced an `InstanceDispatch` that `GetInstanceDispatch` explicitly returns `nullptr`
  for on two paths and that the same function null-checks at two other uses. Reachable whenever the reserved
  overlay queue is absent, in an implicit GLOBAL Vulkan layer - so the fault lands in the host application.
- `CallOriginalPresent`'s Steam external-chain fallback called `presentBypass` unconditionally; every other
  bypass site proves the pointer first. Calling through NULL there is the RIP=0 DEP signature the crash handler
  is built to fingerprint.
- `ue5_console_registry.h` used `uintptr_t{1} << 47`, undefined in the 32-bit hook build. Verified from the
  emitted IR that clang folded `IsPlausibleConsoleObject` to a constant `false` there, so every `[UE5]` console
  variable override was silently inert in 32-bit Unreal titles. `unit_tests.exe` is x64-only and could not
  observe it; the invariant is now a `static_assert` the x86 compile sees.

Two dead subsystems removed: `IPCManager` (487 lines, instantiated nowhere, shipping a permissive shared-memory
SDDL under a comment promising low-integrity support the live inject path never provided), and the Steam-ECL
deferred overlay submission retired in `c4a93a44`, whose enable call was deleted while ~230 lines across 12 files
stayed. The latter also removed a real trap: `ProcessFrameFlow::kSkipSteamFence` was the one flow value whose
original goto label sat mid-function, so returning it skipped everything between - including the per-frame
backbuffer release. `FrameProcessSession` now owns that reference in a destructor.

Also: the crash dump worker copies `EXCEPTION_RECORD`/`CONTEXT` instead of holding pointers into the crashing
thread's stack, which the filter's 5 s wait routinely outlives; libav diagnostics are routed into the session log
with RTMP endpoints redacted, instead of going to a stderr nobody reads; `--verify-runtime` exists because
`--verify` never launches anything (`integration_tests=not_run`, `test_apps=compiled_not_executed`,
`fuzz=not_run`); and a line-length ratchet covers what the 800-line file ceiling cannot see - the splitter had
packed eight declarations onto one 541-character line to satisfy it, and `wgc_capture_internal.h` still carries
one of 2053.

### 2026-09-15 - DirectDraw screenshots now complete in-game; healthy overlay dialogs are not freezes

Gothic II/SystemPack session `20260915_124517` continued presenting DirectDraw frames at 144 FPS after both
screenshot hotkeys, but the hook emitted no `[Screenshot]` line. Request 1 stayed Pending for the controller's
full 15-second wait and failed only as the game exited; the desktop fallback then saved it, while the queued second
hotkey ran after source teardown. The DirectDraw presentation route never consumed the shared screenshot request.

`ComposePresentation` now reads the request at the exact Flip/blit publication boundary and sends owned BGRA8
pixels to the common asynchronous worker. Standard 32/24/565/555 surfaces convert directly; GDI is a one-request
fallback for unusual/non-lockable layouts. Included screenshots run after the native or CPU overlay. Excluded
screenshots restore the reversible CPU composite, run before it, and suppress native D3D7 EndScene drawing for
that frame; an after-EndScene request defers only until the next real frame rather than returning the wrong image.
Recording and screenshot overlay inclusion remain independent, and no encoding or GPU wait enters presentation.

The session's dump was a false positive. It was triggered five seconds after a same-process `#32770` window
appeared on Steam's `gameoverlayrenderer` thread, while the adopted DirectDraw render thread kept heartbeating and
continued for roughly 104 seconds after dump completion. An ordinary persistent dialog now requires the render
heartbeat to reach the configured freeze timeout once a render loop has been observed. Critical `ERR_GFX_STATE`
and pre-render-loop startup dialogs keep their prior immediate/early diagnostic behavior. The documented x86 CDB
path was also corrected to the Windows SDK's 32-bit location under `C:\Program Files (x86)`.

### 2026-09-15 - DirectDraw stopped inventing GPU dependencies and scene boundaries

Gothic II/SystemPack session `20260914_203653` isolated the remaining fallback cost. By the ten-second sample the
CPU renderer had handled 450 composites, but the renderer's stable panel and moving graph shared its first merged
draw command. Command-level invalidation therefore treated every graph tick as a change from command zero, rebuilt
the entire sprite 262 times, and averaged about 14.9 ms of raster work plus 2.6 ms of surface writing. That was the
roughly 29 FPS stall after the older D3D9Ex GPU readback had already been removed.

The fallback cache now compares exact quads/triangles, not renderer commands or hashes. It unions the old and new
bounds of changed primitives, clears that rectangle, and replays the complete draw list clipped to it; unchanged
panel and glyph primitives remain cached even when the renderer batches them with the graph. Axis-aligned quads
use a pixel-centre fill, vertex alpha modulates the atlas, and the composite writes only standard BGRA, RGB565, or
RGB555 layouts. There is no D3D9 helper, upload, GPU readback, or GPU wait anywhere in the DirectDraw overlay path.

Stale pixels had several independent causes. A single backdrop was being reused across flip-chain members; changed
surface bytes and exact Lock/Blt/BltFast rectangles were not sufficient inputs; a shrinking/empty overlay could
leave its old rectangle behind; and RGB16 state compared unquantized colors against quantized surface bytes. The
composite now keeps bounded per-canonical-surface backdrop/last-output state, moves it with Flip/Blt surface memory,
retains overlap across region growth, restores vacated or empty geometry, tracks exact application writes, and
stores the exact RGB565/RGB555 value a later lock expands. A new primary chain clears every pointer-derived proof.

The fast DX7 route is no longer an adapter mode that flips back and forth with the CPU route. The shared adapter
stays on the headless renderer, while a default-on auxiliary D3D7 backend draws immediately before the
application's real `EndScene`. It never issues the synthetic `BeginScene`/`EndScene` pair that preceded both
Steam `gameoverlayrenderer.dll` crashes. A retained all-state block protects the game; initialization happens at a
presentation boundary; offscreen render targets are refused until a real presentation has identified them; and
failure uses bounded exponential retry. Exact native-overlay damage makes later 2D writes repairable without
double blending. Unknown damage and overlay-excluded capture defer safely until application pixels replace it.

Presentation handling now distinguishes an exact pass-through blit from color-key/alpha/ROP/fill transforms,
filters the first incidental front-buffer write after a Flip but accepts sustained no-Flip loading-screen writes,
counts accepted direct-scanout presentations for watchdog/metrics/limiting, and keeps capture read locks out of the
presentation stream. Real DirectDraw exports remain hooked even if d3d8/d3d9 modules are also loaded; only the
synthetic bootstrap is deferred. Recording directly converts standard 32/24/565/555 surfaces into its persistent
D3D11 upload texture, reuses one GDI DIB for unusual layouts, and publishes shared fences/textures only after their
handles validate.

Focused `DDrawPresentPolicyTest`, `OverlayCpuRasterTest`, and `LegacyD3D7VTableAbiTest` coverage passes.
Fresh Gothic II + Steam runtime validation remains pending for native frames, loading screens, overlay visibility
changes, included/excluded recording, and the new timing counters.
