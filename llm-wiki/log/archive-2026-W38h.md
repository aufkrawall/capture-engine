# llm-wiki Log — archive 2026-W38h

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
