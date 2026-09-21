# llm-wiki Log

### 2026-09-21 - CE's own swapchain flag killed Strange Brigade's startup; DX12 sampler overrides reach nothing

Session `20260921_173511`, build 0.1.6757. The game showed
`Can't recover from driver error. Error Code 80070057`, exited with code 1, and never presented a
frame. The 49 MB `FREEZE` dump is CE's watchdog reacting to that modal box, not the event.

**Root cause.** `backbuffer_count` implements its depth by adding
`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` (0x40) to the *application's* creation
descriptor. `dxgi!CDXGISwapChain::ValidateResizeBuffers` XORs the caller's flags with the chain's
creation flags and returns `E_INVALIDARG` on any disagreement in that bit. Proof from the dump: the
chain's stored flags at `swapchain+0x184` were `0x842`, the game's own copy on its stack `0x802`.

The rewrite that hid the flag again lived in `CWrapDXGISwapChain` and in the ResizeBuffers vtable
detour. Steam owned the dxgi Present entry, so CE logged
`keeping the swapchain vtable pristine` and `Preserving real DX12 swapchain identity`, handed the
game the real swapchain, and installed neither. The mutation was unconditional, the compensation was
not - and it was duplicated across six creation paths, which is how they drifted apart.

Fixed in e07c3222: `hook/common/swapchain_flag_policy.h` holds the rule once, the reconciliation
reads the live `GetDesc().Flags` instead of re-deriving intent from the config (a stale "add the
bit" is as fatal as a missing one), a reconcile-only ResizeBuffers claim is installed at the DX12
and DX11 bootstrap independently of the Present-ownership question, and the flag is withheld when
no reconciliation can be established. Validated on hardware in `20260921_175749`:
`ResizeBuffers: Reconciling application resize flags 0x802 -> 0x842`, 10288 frames, clean exit.

**Second finding, from the same session.** Forced AF and `mip_bias=-3.0` did nothing: zero
`DX12 AF:` lines. `PatchIATAllModules("d3d12.dll", "D3D12CreateDevice", ...)` logs `patchResult=0`
because nothing imports it statically, and the injector waits for `d3d12.dll` to be *present* before
injecting (`waitMs=0`, `d3d12=1` on the first poll), so the game had already resolved the export.
Hooking the device CE discovers from the game's command queue (`DX12_PublishNativeLimiterDevice`)
was still too late: in `20260921_175749` the hooks came up at 17:58:01.068 and observed exactly two
static samplers all session, both CE's own overlay root signature.

All `ID3D12Device` objects share one D3D12Core vtable, exactly like `ID3D12CommandQueue` - which is
why `DX12_HookQueueVTable(pQueue)` on the bootstrap queue has always covered the game's pre-existing
queue. The device claim is now made on the WARP bootstrap device too. That claim was removed in
6323ed47 and guarded by a source test; the guarded rule is really "the WARP bootstrap must not become
*application evidence*", and a vtable claim is not that, so the test now asserts the claim exists and
that `MarkD3D12DeviceCreated` still does not. Confirmed on hardware 2026-09-21: forced AF and
`mip_bias` take effect in a DX12 title with the bootstrap vtable claim in place.

`LogSummary` also runs at frame 2000 now, not only at shutdown: "forced AF observed no sampler at
all" is useless information after the process is gone.

### 2026-09-20 - Both fixes validated: Strange Brigade starts, and the sharpen queue switch fired

Session `20260920_225326`, build 0.1.6755, two games back to back. No dump, no `crash.log`, no error
line anywhere, controller exited cleanly.

**The IAT serialization fix holds.** Strange Brigade reached the exact race that killed it an hour
earlier — same module at the same base, same two threads:

```
[22:53:34.845] [T:4D48] IAT: Successfully patched kernel32.dll!CreateProcessA in module 00007FF8B5E00000
[22:53:34.849] [T:4D48] IAT: Successfully patched kernel32.dll!CreateProcessW in module 00007FF8B5E00000
[22:53:34.849] [T:4D48] Late-loaded module steamclient64.dll imports CreateProcess - patched (A=1 W=1)
[22:53:34.961] [T:4C88] IAT: Successfully patched kernel32.dll!LoadLibraryA in module 00007FF8B5E00000
```

It then ran 40 s and logged `PerfLogger: Shutdown, logged 2704 frames`. Previously it died ~50 ms into
this sequence.

**The sharpen queue-change wait fired on hardware for the first time.**

```
[22:55:18.544] Sharpen: DX12 submitting queue changed 0000014AFDE27E90 -> 0000014AD36D00D0;
               chained behind fence value 2690
```

Talos, three seconds after a DLSS-MSFG -> off -> FSR-FG -> off sequence settled. Two genuinely
different queues, one GPU-side `Wait`, and the pass carried on: zero `skipped a frame`, zero
`could not be ordered`, no second `source copy ready` (so nothing was torn down across the switch).
The whole session had exactly three `Sharpen:` lines. This is the hazard `test_sharpen_gpu_timeline.cpp`
could only describe, now observed — and it confirms the two routes really do use different queues once
FG changes state, which the 22:35:12 Talos session could not show because `scQueue == origGame` there.

Overlay row updates stayed continuous across every FG transition; the per-transition counter resets to
`#1` are the overlay state being rebuilt for the new route, not a gap.

**The 104 us vs 12 us `total_us` gap is an accounting artifact, not a cost difference.** `total_us` is
the whole DX12 `ProcessFrame` span (`dx12_hook_process_session_phase1.cpp:13` to
`FrameProcessSession::LogFrameMetrics`). Whether the overlay render is *inside* that span depends
entirely on which route draws the overlay, so **`total_us` is not comparable across routes** — which is
the durable trap here.

Talos drew on the PostSL route (225 `Post-SL overlay SUBMIT`, `render%=100%`) and later the
FFX present-callback bridge; both run from call sites outside `ProcessFrame`. Strange Brigade logged no
`Post-SL overlay SUBMIT` at all — no Streamline or FFX frame generation was active in its run — so its
overlay drew on the normal route, inside the span.

Decisive, within Talos alone: `total_us` was 429 us median over frames 2-20 and 244 us over frames
20-40 while the overlay was still on the **normal** route, then collapsed to 6.5 us at the
`OVERLAY HANDOFF ... route=post-sl prevRoute=normal` at present ~46, and stayed at 6-11 us for the
remaining ~4700 frames. Same game, same second, ~40x from routing alone.

The complement confirms it: Talos's own route counter reports
`[OVERLAY COST] FFX present-callback bridge: ceAvgUs=91 ceMaxUs=223`. 12 + 91 = 103 us against Strange
Brigade's 104 us median, and 223 against its 204 us p99. Same total CE cost, split across two counters
in one title and combined into one in the other.

Ruled out along the way: GPU clocks are the same in both (2865-2940 MHz at 0.920 V), so no downclocking;
`ECL timing/1s` reports `avgMs=0.051` for Strange Brigade against `0.048` for Talos, so CE's
per-ExecuteCommandLists overhead is identical; and `fps_limit_wait_us` is a separate column (9.4 ms
median at Strange Brigade's 90 fps cap), so the limiter is not in `total_us` either. The load difference
is real but irrelevant — Strange Brigade sits at 3-6% CPU and 67% GPU because it is capped, Talos at
10-21% and 11-93%.

### 2026-09-20 - Strange Brigade died in PatchIAT: page protection is process-wide state

Session `20260920_224536`, build 0.1.6754, `StrangeBrigade_DX12.exe`. The game crashed ~50 ms after the
hook thread connected IPC and started `InstallKernel32LoaderHooks`, before rendering a single frame.
`0xC0000005` WRITE to `0x00007FF8B6F6B928`, RIP inside `capture_hook_x64.dll`.

```
capture_hook_x64!IATHook::PatchIAT+0x50d        <- _InterlockedCompareExchangePointer (inlined)
capture_hook_x64!IATHook::PatchIATAllModulesFiltered+0x1a1
capture_hook_x64!InstallKernel32LoaderHooks+0x1a1
capture_hook_x64!HookThread+0x4551
```

`!address 0x7FF8B6F6B928` names it: `steamclient64.dll`, `MEM_IMAGE`, **`PAGE_READONLY`**. CE was doing a
`lock cmpxchg` into a read-only image page.

**Two CE threads were patching the same page.** The log shows them interleaved to the millisecond:

```
[22:46:15.437] [T:5E94] IAT: Patched kernel32.dll!CreateProcessA in module 00007FF8B5E00000
[22:46:15.443] [T:5640] IAT: kernel32.dll!CreateProcessA in module 00007FF8B5E00000 already patched
[22:46:15.449] [T:5640] IAT: Successfully patched kernel32.dll!CreateProcessW in module 00007FF8B5E00000
[22:46:15.449] [T:5E94] <crash>
```

T:5E94 is the hook thread's `PatchIATAllModulesFiltered` sweep. T:5640 is the LoadLibrary hook's late-load
pass — `main_redirect.cpp:PatchLateLoadedCreateProcessImports`, which runs on whichever game thread mapped
the module, here Steam's loader thread mapping `steamclient64.dll`. `CreateProcessA` and `CreateProcessW`
are adjacent thunks in one 4 KB page.

**Root cause.** `PatchIAT` does `VirtualProtect(PAGE_READWRITE)` → CAS → `VirtualProtect(oldProtect)`, and
`g_PatchLock` was taken *after* the unprotect, covering only the `g_PatchedEntries.push_back`. Page
protection is process-wide state, so two of those sequences interleaving on one page destroy each other:
A unprotects, B finishes its own patch and restores `PAGE_READONLY`, A's CAS then writes into a read-only
page. `RestoreIAT` and `ShutdownIATHooks` already held the lock across all three steps — `PatchIAT` was
the one place that did not. Fixed by moving the guard ahead of the first `VirtualProtect`.

The guard starts *after* `TryGetTrackedOriginalForPatchedEntry`, which takes `g_PatchLock` itself.
`g_PatchLock` is a plain `std::mutex` → SRWLOCK under libc++, so a second acquire on the same thread parks
forever — the same trap `ReleaseDX12SharpenResources` hit in 0.1.6741. Making it recursive would hide the
re-entry rather than respect it; `tests/test_iat_patch_serialization.cpp` pins both the ordering and that
it stays non-recursive.

**This is a regression from the unreleased set, not a long-standing bug.** `0ee31cae`
(`fix(ngx): make ngx_ota=off suppress the updater instead of racing it`) introduced the late-load
CreateProcess pass, which is what gave `PatchIAT` a second concurrent caller. Before it, the hook thread's
sweep was effectively the only writer and the missing serialization never showed. It is also why the
crash is timing-dependent and looks title-specific: it needs a module that imports CreateProcess to map
during the sweep. Talos in session `20260920_223512` was fine for exactly that reason.

**The pre-release review missed it.** It covered sharpen, the Vulkan layer, the inline-hook engine and the
release tooling, but never asked what the new NGX-OTA late-load path races against. A new call site for an
existing global-state mutator deserves that question by default.

### 2026-09-20 - Pre-release review of v0.1.6652..HEAD: the sharpen pass has two submit queues

Release-readiness review of the whole unreleased set (264 files, ~33k insertions). `--verify` passed
clean at 0.1.6748 (unit tests, x64 ASan/UBSan, lint) before any change; `coverage.integration_tests`,
`coverage.fuzz` and `coverage.test_apps` are `not_run`/`compiled_not_executed` as always, so nothing
here is evidence about a real present path. Sharpen still has **no hardware run at all**.

**The finding that matters: `D3D12Pass` assumes one submitting queue and there are two.**
`dx12_hook_process_session_draw_main.cpp:356` submits on the game's queue;
`dx12_hook_postsl_render_submit.cpp:44` submits on `submittedQueue`, which `Chunk3` resolves to the
game's queue *or* Streamline's `scQueue`. A DLSS-G activation switches CE between the two routes inside
one swapchain generation. One `ID3D12Fence` signalled from two queues gives completion values that are
not ordered against each other, so `GetCompletedValue()` can pass a value whose work is still running:
the allocator recorded for it gets `Reset()` under the GPU, and the single `sourceCopy_` texture is
written by one queue while the other still reads it.

The fix is a GPU-side `queue->Wait(fence_, fenceValue_)` issued once per switch, which re-establishes a
single ordered timeline. No CPU stall, no per-frame cost. If the `Wait` is refused the pass resets
itself rather than submitting into a timeline it cannot reason about.

Two more in the same file, same root cause (D3D12 keeps no reference for a submitted command list):
- `EnsurePipelineState` released the old PSO the moment `sharpen=cas` became `sharpen=rcas`. Replaced
  objects now go on a deferred-release list keyed by fence value.
- `EnsureSourceCopy` rewrote the one **shader-visible** SRV descriptor. A descriptor cannot be deferred
  the way a resource can, so that rebuild now waits for the timeline to drain by *skipping frames*,
  never by blocking the present thread. (`DX12OverlayState::Cleanup` already drained the resize path,
  so this is the belt to that suspenders.)

**Vulkan sharpen:** `vkResetFences` succeeded and `vkQueueSubmit` then failed left the slot marked in
flight against a fence nothing would ever signal — permanently retired. `kSharpenSlotCount` (3) of those
and sharpening was off for the session, logging only "every command buffer is still in flight". Also
`sourceInitialized` was set at *record* time, so an aborted submit made the next frame's barrier declare
`SHADER_READ_ONLY_OPTIMAL` for an image still in `UNDEFINED`; and `layer_sharpen_g_States[device]`
inserted an entry before the dispatch-table null check.

**New:** `hook/common/sharpen_gpu_timeline.h` holds those rules as pure logic, the way
`sharpen_policy.h` holds the decision rules — ordering bugs are exactly what can be checked without a
GPU. `tests/test_sharpen_gpu_timeline.cpp` covers them; the failed-submit ring case fails on the
previous revision.

**Reversed on inspection:** `RestoreOwnedEntryPatch` taking the `kAcceptSuspendedSet` fallback looked
like an unwarranted relaxation of *un*patching. It is the opposite. `Remove`/`RemoveAll` both log
"leaving it installed" on refusal and keep the entry, so a refused restore leaves CE's jump patched into
a game CE is unloading out from under. `hook_patch_transaction.h` already argues the residual race is
identical in both modes because `IsRangeSafe()` is what actually gates the write. Behaviour kept; the
relaxed path is now reported in the log (after the transaction has resumed every peer — logging inside
the suspended window can deadlock). That report pushed `inline_hook.cpp` to 825 lines and the file-size
preflight refused it, which is the gate working: the live-code writes moved into
`inline_hook_entry_patch.cpp` (213 lines) and `inline_hook.cpp` came back to 640. The boundary is real
rather than arbitrary — one unit writes running code under quiescence, the other owns the bookkeeping
around those writes — and `RestoreOwnedEntryPatch`/`InstalledEntryBytesMatch`/`OwnsInstalledEntryBytes`
lost their internal linkage to `inline_hook_internal.h` to cross it.

**Release process.** The build number is a local counter (`build_io.py:bump_and_write_build_version`),
so the release version does not exist until the runner has built it — `## Unreleased` is therefore still
un-promoted when the tag is cut, and the `--generate-release-notes` fallback to `## Unreleased` is the
normal path, not a degraded one. Promotion stays the operator's post-release step; the job summary now
prints the exact command. What is closed is the failure mode: once `## Unreleased` states
`Changes since [v<tag>]`, the generator refuses to publish those entries under that same version again.
Also: notes are generated *before* the tag is pushed (a generator failure used to strand a tag), and
`--promote-release --version <v>` — the form `changelog-guidelines.md` documented — aborted with
"expected one argument" and is now accepted alongside `--promote-release <v>`.

**Changelog gaps found and filled:** `561b286e` (config save respawning inject, ~7 s overlay blackout,
desktop "NOT RECORDING" warning, present-thread deadlock) had no entry at all; neither did the WER
dialog suppression in `4415c786`. The sharpen entry named `sharpen_intensity` as the strength control
while `config.ini.template` ships `sharpen_contrast`/`sharpen_amount` — and intensity is the mix weight,
not the strength. `dlss_fg_preset` (0c2a299e) remains absent from every release section; pre-existing,
still not backdated.

**`FpsLimiterTest` flakiness, addressed but not eliminated.** Six `EXPECT_LT(elapsedMs, 100.0)` upper
bounds were failing under host load on clean HEAD too (~1 in 6 full-suite runs, a different test name
each time). What those bounds exist to catch is a blocking wait on the remote-limiter release event,
which would cost that event's whole timeout — hundreds of milliseconds, not tens — so 100 ms was never
the discriminator, it was just tight enough to catch an ordinary scheduling stall. Raised to 500 ms with
the reasoning written at each site, and `elapsedMs` is now a `RecordProperty` so a genuine slowdown is
still visible. `GateEveryPresentStaysNonBlockingWhenInactive` additionally gained the exact,
load-independent form of its claim: `EXPECT_EQ(limiter.GetLastWaitUs(), 0)`. 5/5 clean repeats after.

This is a weaker timing assumption, not the absence of one. The structural fix is a counter for "waited
on the remote release event" that every one of these tests could assert zero on; note also that
`releaseEventName`/`requestEventName` currently have **no first-party consumer** outside
`shared_memory_layout.h` and this suite, so these tests are guarding a path that would have to be
reintroduced. Worth resolving one way or the other. b53f78f6 did the same conversion for
`SmartWait_Accuracy` and is the pattern to follow.
