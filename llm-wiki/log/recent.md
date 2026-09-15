# llm-wiki Log

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

### 2026-09-14 - What the instrumentation proved, and the CPU/GPU dependency it pointed at

Session `20260914_193129` (0.1.6598) is the first one that could answer anything, and it retired both remaining
theories at once. `ok=361` of `composites=361`, `stageFailed=0 writeFailed=0`: **every composite succeeded**. And
`[Overlay] Rows changed` fired 23 times in the whole session with `valid=GV-g-Pfk-` constant and a real GPU
reading in every one - only the very first line, before the adapter resolved, carried a `--`. **The sensor values
were never flapping.** The counters also reconcile exactly for the first time: 31 flips + 62 blits + 235 unlocks
= 328 presentations, of which 198 were CE's own re-entry (the composite locks and unlocks through the same
hooks), leaving 130 real ones and `composites=130`.

So the overlay is drawn, correctly, on every presentation, from stable data - and it still strobes. What is left
is *when* it lands. The composite was a CPU/GPU round trip per presentation: read the surface region, upload it,
blend on the GPU, and `GetRenderTargetData` it back - a call that blocks the render thread until the GPU has
caught up. During the intro videos and loading screens the application updates the *scanout* surface about ten
times a second, so that sequence runs between the application's own draw and the overlay reappearing, on a
surface the display is scanning out live.

There is no GPU path from a D3D9Ex device into a DirectDraw surface, so the composite cannot stay on the GPU.
`hook/common/overlay_cpu_raster.cpp` rasterizes the shared draw list instead - the same vertices, indices and
GDI font atlas the GPU backends consume - into a premultiplied BGRA sprite, and
`BlendOverlaySpriteIntoSurface` blends that into the locked surface in one pass.
**The GPU, the readback and the synchronization are gone from that route entirely.**

Cost was the first question, not the last: the sprite is cached against an FNV-1a revision of the built geometry
(`OverlayAdapter::GetLastDrawDataRevision`), so a presentation that changed nothing reuses it; the edge functions
are stepped along each scanline rather than solved per pixel; and the mix line now reports `raster=`,
`spriteReuse=`, `compositeAvgUs=` and `compositeMaxUs=` so the cost is a measured number rather than a claim.

One rasterizer detail is worth keeping: `AddQuad` emits two triangles sharing a diagonal, so the top-left fill
rule is load-bearing, not pedantry. Without it every pixel on that diagonal is covered by both triangles and
blended twice - `AHalfTransparentQuadIsStoredPremultiplied` caught exactly that, 192 where 128 was expected.

Still true, and still the better answer: drawing with the application's own Direct3D 7 device
(`legacy_d3d_native_overlay`) needs no pixels on the CPU at all. It stays off by default until the Steam-overlay
crash can be attributed, which needs the WoW64 dump gap closed first.

Built and verified at 0.1.6604. **Hardware run pending.**

### 2026-09-14 - A frozen backdrop, a vacated strip, and the diagnostics that were missing

Session `20260914_192142` (0.1.6594) reported both symptoms unchanged, which retired the previous round's two
hypotheses. `backdropReuses=179` of `composites=223` proves the backdrop mechanism was live, so the double-blend
it removes was not what was being seen; and the sensor service published cleanly throughout (one adapter
resolution, no rejected publications), so the value strobing is not the publisher flapping.

**The backdrop reuse was itself wrong, and worse than what it replaced.** Reusing a saved backdrop for every
repeat composite into the same surface freezes the application's pixels under the overlay for as long as the
reuse lasts. On a loading screen - where the application animates and rarely flips - that meant CE wrote stale
content into the overlay's rectangle every frame. The rule now requires proof that the region is untouched: CE
keeps the exact pixels it last wrote there, reads the region, and compares. Byte-identical means the application
has not drawn into it since and the region is still carrying CE's own output, so the saved backdrop is used.
Anything else - a different surface or rectangle, a flip or blit-present republishing the image, or content that
simply differs - means what was just read *is* the application's frame and becomes the new backdrop. 16-bit and
GDI writebacks leave nothing comparable, so they always take the fresh read.

**A shrinking overlay left a stale strip.** `EnsureCompositeRegionResources` follows the overlay's own bounds, and
the log shows them oscillating across a 64-pixel alignment step (512x320 -> 448x320 -> 512x320 -> 512x384) as
value widths change. When the rectangle shrank, the strip it vacated kept the previous composite and nothing ever
repainted it. `ExpandToPreviousComposite` now unions the new rectangle with whatever CE wrote into that surface
last time.

**The diagnostics could not answer either question, which is why three rounds of hypotheses were wrong.**
`composites` counted attempts, not outcomes, and 188 of 411 presentations in that session vanished into the
recursion guard uncounted. The mix line now carries `ok=`, `noGeometry=`, `stageFailed=`, `writeFailed=` and
`reentrant=`, so a composite that runs but never reaches the surface is distinguishable from one that is never
attempted. And the overlay now logs its own rows: `[Overlay] Rows changed #N: cpu='...' gpu='...' gpuClk='...'
cpuClk='...' valid=GVcgpPfkK` whenever any of that changes, which is the only place a row alternating between a
reading and `--` was ever visible.

Built and verified at 0.1.6598. **Both symptoms are still open**; this round makes the next session decisive
rather than claiming a fix.

### 2026-09-14 - The overlay was being blended over itself, and an idle process is not an unreadable one

Session `20260914_190240` (0.1.6590) had two symptoms with two separate causes.

**The composite blended the overlay over its own previous output.** The DirectDraw composite is a
read-modify-write: it reads the target region, blends the overlay over those pixels and writes the result back.
That is only correct while the pixels it reads are the application's. A flip publishes a freshly rendered image,
so the region read after one is always clean - but consecutive writes into the *same* scanout surface with no
flip between them are not, because the application may have changed only part of the surface and left the
overlay's own pixels in place. A translucent overlay then darkens a little more with every repetition, and the
next flip restores a clean one. Gothic II does both shapes: 86 flips and 11 front-buffer writes a second during
gameplay, and about ten writes per flip through the intro logos, which is the ratio that made it look like the
values themselves were flashing.

The previous attempt - `ScanoutWriteIsPresentation`, suppressing front-buffer writes while the chain was still
flipping - treated the symptom and created a worse one: a loading screen that flips occasionally between its
writes left every write as "the first after a flip", so the overlay was never restored for the whole load. That
rule is gone. The composite now keeps the application's own pixels for the region it is compositing
(`CompositeBackdropIsReusable`): read once for a given surface and rectangle, reused for every repeat composite
into the same place, discarded as soon as a flip or a blit-present brings a newly produced image. Repeat
composites become idempotent, so a front-buffer write no longer has to be suppressed to avoid strobing, and a
loading screen composites on every write again. The mix line reports `backdropReuses=`.

**A process with no GPU-engine counter instance is idle, not unreadable.** Windows publishes a
`\GPU Engine(...)` instance for a process only while that process has work on that engine, so the instances come
and go. `host_metrics.cpp` set `gpuUsageValid` only when at least one instance matched, so a poll that found none
published the GPU row as unavailable and the overlay drew `--`. Through the intro logos, where the game barely
touches the GPU, that flapped at the poll rate. A resolved adapter plus a counter query that succeeded is a
readable sample; no matching instance means the process did no GPU work in that interval, which is 0%
(`metrics_policy::GpuLoadIsReadable`). Unreadable is now reserved for an unresolved adapter or a failed query.

VRAM usage has the same instance behaviour but no truthful zero - a process with a resolved adapter is using
some - so `VramUsageIsReadable` holds the previous reading across a bounded run of missing samples
(`kVramUsageMissingSampleTolerance`, five polls) and only then gives up.

Built and verified at 0.1.6594. **Hardware run pending.**

### 2026-09-14 - The native D3D7 overlay is opt-in, and a front-buffer write is not a present

Two more Gothic II runs settled two open questions.

**The native Direct3D 7 overlay is what crashes this title.** Session `20260914_183948` (0.1.6587, with the
backend/route match rule in place) crashed again, byte-identical to `20260914_182411`: `movdqa xmm0,[esi]` at
`gameoverlayrenderer.dll+0xB8EA0`, ESI null, `EDX=0x0D ECX=0x24` - Steam's inlined SSE `memcpy` copying 1700 bytes
from nothing. So the previous attribution was wrong: the wrong-backend behaviour was a real bug and worth fixing,
but it was not this crash. What both crashing sessions share, and what the clean `20260914_180020` session did
not have, is the native backend running at all. `legacy_d3d_native_overlay` in `[Graphics]` now gates it and
defaults to **off**. The composite draws the same overlay on every title; the native path is an optimization that
has taken a co-resident Steam overlay down twice and cannot be attributed further from the dumps available.

**CE's crash dumps carry no 32-bit stack.** Both dumps are written by the x64 external helper against a WoW64
target with `kRichCrashDumpType`, which captures the *native* thread stacks - `wow64cpu!CpupSyscallStub` and
nothing below it. `dds` over the recorded 32-bit ESP returns `????????` in both. That is why neither crash could
be attributed past the faulting instruction, and it will be true of every future WoW64 crash until the helper
either dumps full memory for such targets or adds a memory callback carrying each thread's 32-bit stack range.
**Open, not fixed.**

That session also showed the route switching five times in eight seconds, rebuilding the backend's Direct3D 7
objects inside the Flip detour each time, because the application's render target legitimately alternates. A route
change now has to hold for 45 consecutive presentations before it is acted on.

**A write into a flip chain's front buffer is not a presentation while the chain is being flipped.** Session
`20260914_184850` (0.1.6588, composite route, no crash) still flickered, and the presentation mix says why:
`flips=932 ... scanoutUnlocks=327`, about eighty-six flips and eleven front-buffer writes a second. Each of those
writes composited the overlay into the surface the display was scanning out, for a frame the next flip
immediately replaced - the same race the flip fix removed, re-entered through the loading-screen rule. The
front-buffer rule is right for a loading screen, where nothing flips; it is wrong while flips are arriving.
`ScanoutWriteIsPresentation` now judges that by structure rather than by a clock: a flip resets the run of
scanout writes, a write extends it, and only a run that reaches two without a flip answering it is the
presentation. Gameplay never reaches two; a loading screen reaches it on the second write. The mix line reports
`scanoutWritesLeftToTheFlip=`.

Built and verified at 0.1.6590. **Hardware run pending.**
