# llm-wiki Log

### 2026-09-18 - The NGX OTA store was quietly beating the configured DLSS runtime

The user asked whether CE had collided with NVIDIA's NGX updater after seeing a pile of
`nvngx_update.exe` processes hang. It had not, and establishing that turned up the more interesting
problem.

**Not CE's doing.** `nvngx_update.exe` imports only KERNEL32, SHLWAPI, ADVAPI32, SHELL32, ole32,
bcrypt, Normaliz, CRYPT32 and WS2_32 - no USER32, no vulkan-1, no d3d/dxgi - so none of CE's
machine-wide surfaces can reach it. CE injected exactly once that session (AlanWake2.exe, PID 17468),
and its `CreateProcessA/W` hook passes non-whitelisted children through with unmodified flags. The
pile-up mechanism is NVIDIA's own: `_nvngx.dll` coordinates updater runs through the global named
object `Global\NGX_Updater_update_0`, so one instance wedged on its network fetch queues the rest.

**What the session did reveal.** Alan Wake 2's Streamline resolved its `sl.common` core to
`C:\ProgramData\NVIDIA\NGX\models\sl_common_0\versions\134656\files\1B0_E658703.dll` - the driver's
OTA store - so CE correctly refused all six `sl.*` redirects rather than build a version-mixed stack,
and the user's configured `npi\sl` runtime never loaded. The only evidence was one line in
`hook_debug.log`.

**A correction worth keeping.** The first analysis framed the unelevated WMI process-start fallback as
an injection-latency problem (`age=419.522 ms`). It is not: CE's hooks were fully installed at
19:53:39.98 and the game did not create its real D3D12 swapchain until 19:53:44.273, ~4.95 s later.
The `sl.interposer` that did beat CE is a **static import of the game exe**, resolved during process
initialization - no attach-to-running-process injection can beat that at any detection speed. The WMI
fallback's real cost is machine-wide load, not lateness.

**Shipped in 0.1.6654 (ABI 59):**

- `[DLSS] ngx_ota=default|off|on`. `off` refuses the `nvngx_update.exe` launch *and* clears
  `eAllowOTA|eLoadDownloadedPlugins` from the game's own `slInit` preferences (the refusal stops new
  downloads; the preference strip stops already-downloaded plugins loading). `on` forces the
  opposite and stands CE's own `nvngx_*`/`sl.*` overrides down so the driver's OTA files are what
  loads. `default` is inert, and an unrecognized value falls back to it rather than to either forced
  mode. Nothing touches the registry, NVIDIA's config files, or the model store.
- `[DLSS] ngx_log=default|off|on|verbose`, routing NGX's own log into the CE session directory via
  `__NGX_LOG_LEVEL` / `__NGX_LOG_PATH_OVERRIDE`. Honoured at any CE log level, `none` included.
- The kernel32 loader/CreateProcess hooks now install in `DllMain` before the graphics IAT work,
  closing the 330 ms window in which mapped modules were unredirectable.
- A refused runtime override is published hook -> host (`SharedMemoryLayout::runtimeOverrideStatus`,
  PID-tagged, first-refusal-wins) and is reported by the inject process, instead of living only in
  `hook_debug.log`.
  - **Corrected the same evening (session `20260918_221342`).** The first version raised a Windows
    tray balloon and logged `Configured DLSS/Streamline override did NOT apply`. Both were wrong.
    The refusal concerns exactly one mechanism - the `sl.*` plugin set behind `streamline_dll_path`
    - while the NGX runtimes behind `dlss_sr_dll_path` / `dlss_fg_dll_path` / `dlss_rr_dll_path` are
    a separate, generation-independent override that is unaffected. In that session all three loaded
    from the configured `npi\sl` folder (`Loader: runtime module loaded: nvngx_dlss.dll -> ...\npi\sl\...`,
    and `nvngx_debug.log` confirms `Detected Version ... v310.9.1`) while only the `sl.*` set was
    refused, so the message told the user their whole configuration had failed when the part they
    cared about had worked.
  - The balloon is gone entirely (`TrayIcon::ShowNotification`, the registered window message and
    the inject-side `PostMessage` are all removed). Interrupting a running game with a desktop
    notification is the wrong surface for something that is not actionable mid-session, and CE
    refusing to build a version-mixed Streamline stack is correct behaviour rather than a fault -
    so the remaining report is one `LogInfo`, not a `LogWarn`, and it names the mechanism.
- The unelevated process-start fallback is `ce::process_start::Poller`, one
  `NtQuerySystemInformation` sweep every 250 ms, replacing the WMI `__InstanceCreationEvent WITHIN
  0.5` query that made WmiPrvSE materialise every process instance twice a second.

**Safety of the `slInit` route.** Three guards, because reaching into a game's argument struct is
the risky part: 2.x only (1.x has a different signature and Preferences layout), `BaseStructure`
GUID + version identity checked against the SDK header CE compiled with, and a modified **copy** is
forwarded - the game's own memory is never written. Any guard failing means the call is forwarded
exactly as it arrived.

**Explicitly rejected:** writing NVIDIA's registry, `nvngx_config.txt` or
`nvngx_ota_updates_config.txt`, and driving `nvngx_update.exe` with its undocumented CLI flags
(`-forced_update`, `-force_add_update`, `-bootstrap`, ...). Machine-wide, persistent, affects other
applications - and the in-process CreateProcess route gets the same outcome deterministically.

**Unvalidated on hardware:** all of it. No game run since the change.


### 2026-09-16 - One capture, two files: combined HDR + SDR screenshots, concurrently

The user asked for a screenshot option that saves an HDR *and* an SDR variant at once, in both the
WGC and the inject path, ideally without paying for two screenshots — and then added the obvious
constraint: no HDR file when Windows and the game are not in HDR mode.

The pipeline made this cheap, and it is worth recording why. Both routes already converge on a single
`RawScreenshot` before anything is encoded: the inject hook writes a raw payload the controller reads
back, and WGC/DXGI readback fills the same structure. `SaveRawScreenshot` is the only encoder, and
the source's declared color contract - not its storage precision - already decides which encoder runs.
So "both variants" is one capture encoded twice, with nothing to add in any backend, and the two
files are pixel-identical in origin by construction. No hook, no ABI, no capture code was touched.

What the change actually consists of:

- `ScreenshotOutputColorSpace::SourceAndBt709` and `[Screenshot] color_space=both`, now the default.
  `auto` and `bt709` keep their meanings.
- `SaveRawScreenshot` now reports a `ScreenshotPublication{hdrPath, sdrPath}` instead of one path,
  because a single path cannot describe a pair. `IsHdrScreenshotSource` is the public predicate for
  "is there a second variant at all": R10+BT.2020/PQ or FP16+linear scRGB HDR, nothing else. An SDR
  presentation stored in ten bits or FP16 is still SDR and still yields exactly one PNG under every
  policy - the combined mode must never fabricate an AVIF out of SDR pixels.
- The two encodes overlap. The AVIF branch goes to the worker thread because it touches no COM; the
  WIC PNG writer stays on the caller's initialized apartment. A `std::thread` that cannot be created
  logs and falls back to sequential encoding rather than silently dropping a variant. Since the AVIF
  pipeline is roughly an order of magnitude longer than the tone-mapped PNG (about 950 ms versus
  about 250 ms at 4K), the pair costs little more than the HDR-only case.
- Paired naming: both variants publish through one `OutputNameSeed`
  (`ce::capture_output::MakeOutputNameSeed()`, with `PublishToNewPathWithSeed` promoted from the
  private/ForTesting surface), so the pair shares a stem and differs only by extension. Different
  extensions cannot collide under a shared seed.
- A partial result is a failure and says which half failed, but the half that did publish is already
  a complete atomically renamed file and is kept. Deleting a good screenshot to make a bool tidy
  would be the wrong trade.

Regression coverage: HDR-source classification across all five format/encoding pairs, combined
publication of a packed-PQ and of an scRGB capture (paired stem, exactly two files, decodable
10-bit 4:4:4 PQ AVIF), combined publication of an SDR source producing exactly one PNG, and the
config parse/default. No runtime validation on a real HDR game yet - that stays a manual check.

### 2026-09-16 - Four startup stalls that were CE's, found by auditing rather than by a log

The user reported Gothic II sitting black for ~10 s after launch and, when the first analysis blamed a
cold page cache, kept insisting CE can *randomly* slow a start. Both readings turned out to hold
something. The 10.6 s in `20260916_172304` really was a cold standby list - the machine booted at
17:22:23 and that was the first launch of the boot session, the game lives on `H:`, an **external M.2
NVMe over USB**, and the same launch 7 minutes later took 0.49 s. CE's own flip counter proves it was
not in the way: `flips=1` before the gap and `flips=2` after, so the game issued exactly one
presentation call across it. Note the confound that makes this indistinguishable in ordinary use: CE
autostarts ~40 s after boot, so "first launch after a reboot" and "CE attached" are the same event,
and no amount of normal play separates them.

But auditing the startup path for the user's claim found four real defects, all with variable cost:

1. `ThreadQuiescence` enumerated **every thread on the machine** per inline entry patch, peers
   suspended throughout - 37-533 ms each, 279-1249 ms per launch, 5x run-to-run spread. The trace
   isolates it exactly: nothing but `WriteOwnedEntryPatch` sits between the last `WriteJump:
   Verification` line and `InlineHook: Prepended CE at`. Now `NtGetNextThread`, process-scoped.
2. The temp-swapchain bootstrap built a WARP D3D12 device in a DirectDraw7 game - 650-1430 ms per
   launch, bimodal, and it loads the WARP runtime into the loader lock while the game is loading its
   own DLLs. Now refused for legacy-presentation processes.
3. `ResolveDirectDrawTargetWindow` -> `FindAuxiliaryProcessWindow` -> `GetWindowTextA` on the game's
   **render thread**. `GetWindowText` on a window owned by the current process sends `WM_GETTEXT` and
   blocks with **no timeout**; the fallback is reached exactly at startup, when the UI thread is busy
   loading and the game is not foreground yet. This was the only unbounded one.
4. The freeze watchdog did the same unbounded send twice a second for the whole session - a watchdog
   that can itself wedge on the thread it is judging.

3 and 4 now share `ce::window_text::ReadWindowTitleBounded` (`hook/common/window_text_safe.h`), a
50 ms `SendMessageTimeoutA(WM_GETTEXT, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT)`; a timeout is an empty
title, which only changes dialog identity in the case that used to hang. Same-thread windows still go
through `GetWindowTextA`, where there is nothing to wait for.

**Cold-cache reading CONFIRMED 2026-09-16** by a cold-boot launch with CE not running: also slow.
Three-for-three against the Windows boot log (System event 6005/6009): boot 17:22:33 -> 17:26 launch
10.63 s, 17:33 launch 0.49 s, 18:14 launch 0.55 s; boot 19:11:14 -> 19:12 launch 10.62 s. Cold first
launch is ~10.6 s every time, warm ~0.5 s every time, independent of build and of CE being attached.
`Get-WinEvent -FilterHashtable @{LogName='System';ID=6005}` dates every boot and settles this faster
than any log analysis.

**The four fixes are hardware-VALIDATED** in `20260916_191145` (0.1.6651): `DDraw hooks installed
(init=0.5 ms)` against 77-523 ms before, the WARP bootstrap refusal logged once and skipped, one
quiescence over 8 ms for the whole launch (`15 ms over 2 pass(es) route=process-scoped`) against seven
totalling 279 ms, and CE's hook-install phase (DllMain -> DDraw hooks done) 1.246 s -> 0.645 s. The
user saw no regression across several other games. Nothing here was reverted: each fix is correct on
its own terms regardless of what caused the original report.

**Not proven**: none of these fired during the 10.6 s in `20260916_172304` - CE ran no code on that
render thread across the gap. They are genuine random-stall bugs; they are not that stall. Regression
coverage in `tests/test_startup_stall_hazards.cpp`, including a parked non-pumping window thread that
can only be answered by the timeout, and an equivalence test between the two thread walks. See
`dx12-injection-bootstrap.md` for the invariants.

### 2026-09-16 - The nested presentation was the game's screen flip, and CE was dropping it

Gothic II session `20260916_021049`: after the intro videos the picture stopped updating - the 2D menu
stayed on screen while the loaded 3D scene ran with its audio. The cycle report added the day before
named the caller on its first real occurrence:

    Re-entered from gameoverlayrenderer.dll+0x76ACC; CE's saved original is DDRAW.dll+0x37C50;
    the entry point recorded before CE patched the slot was DDRAW.dll+0x37C50

Three things follow from that one line. CE's saved original is **genuine DirectDraw**, so the previous
session's reading - CE and Steam each holding the other's vtable pointer - was wrong. The other
injector sits *below* CE: CE calls `DDRAW+0x37C50`, that injector owns the entry, and it re-issues the
presentation through the surface vtable, which is CE's detour. And that is precisely why
`20260916_013230` recursed 32,768 levels deep when CE answered the cycle with the entry point recorded
before patching the slot - the same address, the same patch.

The nested Flip was on `0da10fa0`, the surface CE itself tracked as the primary. So the guard's stated
cost, "one dropped presentation", was the whole frame, every frame: `flips=14633` with roughly one
refusal each, and nothing reaching the screen. A guard that cannot recurse is not automatically a guard
that is safe.

A nested presentation is now answered by running DirectDraw's own implementation past the foreign entry
patch, through a bypass trampoline built from the module's on-disk bytes - `CreateBypassTrampoline`,
which CE already used for a patched `dxgi!Present` and had never applied to DirectDraw. The other
overlay has already drawn by the time it delegates, so nothing is cut out of the chain. Bounded: once
per outermost presentation, refusal below that, refusal when the entry carries no `E9`/`FF 25` patch at
all. The probe is deliberately not cached negatively, because the injector can hook after CE does.

Also this session: the stack-only freeze dump proved itself on hardware - 273 KB and 35 WoW64 thread
stacks for the game's own alt-tab dialog, against 30 MB the run before - and the Surface4 split turned
out to have broken a source-policy test that counted presentation owners in one unit. The `--verify`
that would have caught it had been interrupted; it reads the whole hook family now.

### 2026-09-16 - Working through the hand-off's open items

Six follow-ups from the Gothic II session, none of them needing a new hardware run to justify.

**The 8.7-exabyte VRAM reading was a format string.** `UpdateVRAMTotal` printed DXGI's
`DedicatedVideoMemory` - a `SIZE_T`, four bytes in the 32-bit hook - through `%llu`, so the conversion
read four bytes of the neighbouring stack as the high half. The stored and reported value was always
the correct 11943 MB. The same loop matched its adapter on an uninitialised descriptor whose
`GetDesc1` result was unchecked; a failed call left the LUID it compares in play.

**The DirectDraw bootstrap was leaking a primary surface.** It creates an `IDirectDraw7`, a primary
surface and a D3D7 device purely to reach the vtables it patches - the patches belong to `ddraw.dll`
and outlive the objects - but the surface was never released, which kept the `IDirectDraw7` alive
with it, on a window the bootstrap destroys on its way out. Whether that is what made Gothic's
post-alt-tab `CreateSurface` return `DDERR_UNSUPPORTEDMODE` is still **not established**; the cheap
experiment (one alt-tab, CE not injected) is still the next step. Both prototype sentinels are
cleared with it - the Surface4 one already named a released object, and a pointer comparison against
a freed surface can exclude one of the application's own surfaces.

**The composite's hitches were the access pattern, not the amount of work.** `writeAvgUs=3210` with a
44 ms peak came from reading and storing the locked surface one pixel at a time: uncached reads,
interleaved with stores that break write combining. Rows are now copied out with one `memcpy`,
composed in cached memory, copied back with another. `ComposeCompositeSpan` holds the arithmetic
unchanged, and a test runs it against the loop it replaced over the same span in all four
proof/restore combinations. Unmeasured on hardware.

**A freeze the application explains itself no longer costs 30 MB.** When the `#32770` window belongs to
the thread CE monitors for presents, that thread is running the dialog's modal pump - which is why
the heartbeat went stale - so the cause is established and the memory adds nothing. That case now
writes thread stacks, thread info and modules; the WoW64 stack ranges the dump callback contributes
are still included, so a 32-bit target stays walkable. `--dump-helper-scope=stacks` carries it to the
external helper. `ERR_GFX_STATE` stays full and immediate.

**The re-entry cause is still unidentified, and still not guessed at.** What was added is the one
observation CE can make directly: at install time, when the pointer `VTableHook::Create` keeps as
"the original" is code from outside the module owning the vtable, CE is joining another injector's
chain - the precondition for the cycle - and now says which module. It cannot stop that injector
reinstalling after CE, so nothing about hooking behaviour changed.

**`ddraw_hook_detours.cpp` was seven lines from the ceiling.** The Surface4 generation moved to
`ddraw_hook_detours_surface4.cpp` and the shared classification to
`ddraw_hook_blit_classification.h`: 413 + 209 + 193 lines, no behaviour change. The re-entry source
policy now reads the whole DirectDraw family's logical translation unit, so it still counts all nine
guards.

One hand-off item was already done: `DetourDirectDraw7CreateSurface` logging is rate-limited to the
first four creations, every 256th, and every primary-surface creation (44 lines for ~280 surfaces in
`20260916_014133`) - the four `DDERR_UNSUPPORTEDMODE` failures are visible precisely because they are
primary-surface creations.

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
