# llm-wiki Log

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

### 2026-09-20 - Automated changelog handling and ADHD-friendly guidelines

Automated `CHANGELOG.md` validation, extraction, and GitHub release notes generation:
- **Pre-commit changelog rule in `AGENTS.md`:** Agents must update `## Unreleased` in `CHANGELOG.md` before
  committing code changes, preventing changelog drift.
- **`llm-wiki/changelog-guidelines.md`:** Established core principles:
  - Transparent application issue attribution: every entry must state the user-facing defect, crash, or
    capability solved rather than purely internal code mechanism.
  - ADHD-friendly scannability: bold lead-in anchor on every bullet (`- **<Anchor>** <Details>`), standard
    categories (`### New`, `### Improved`, `### Fixed`), short punchy sentences scannable in 15 seconds.
  - Continuous incremental updates during dev loops under `## Unreleased`.
  - Parity between `CHANGELOG.md` and GitHub release tag notes.
- **Automation tooling (`tools/manage_changelog.py`):** CLI and library for `--validate`, `--check-worktree`,
  `--extract-notes`, `--generate-release-notes`, and `--promote-release`. Validated continuously via
  `tools/tests/test_changelog.py` in `tools/python_tool_self_tests.py`.
- **GitHub Actions integration:** `.github/workflows/release-stable.yml` now generates release notes directly
  via `python tools/manage_changelog.py --generate-release-notes --version $version --commit $env:COMMIT --output $releaseNotesFile`
  and passes `--notes-file` to `gh release create`, ensuring published release tag notes feature full issue highlights.

### 2026-09-20 - [StartupPerf] was reading a different object than the one being written

Session `20260920_211603`, build 0.1.6746, Talos Reawakened, ~90 s, clean exit. The session itself was
healthy - DLSS MSFG 3x, overlay on the PostSL route at `render%=100%`, ~117 fps output over ~39 base,
1% low ~100, CE's own per-frame cost 18.6 us average, zero overload flags. One line did not fit:

```
[StartupPerf] Controller startup: VulkanRegistration=0.000 ms, ..., TotalToReady=36055.158 ms
```

The log timestamps put controller startup at `21:16:05.103` -> `.245`, i.e. **142 ms**, and the tray and
Vulkan registration visibly took ~11 ms and ~19 ms - so two measured fields read zero and the total was
254x too large.

**Root cause: an `inline` variable silently lost external linkage.** `main_internal.h` declared

```cpp
namespace { struct ControllerStartupTimingState { ... }; }
inline ControllerStartupTimingState main_g_ControllerStartupTiming;
```

An unnamed namespace gives the *type* internal linkage, and the variable inherits it. The `inline`
therefore merges nothing: every translation unit gets its own object. `main_entry.cpp` filled its copy,
`main_recording.cpp` read a zero-initialized one, and `TotalToReady` degenerated into
`Log_GetQpcUs() - 0` - an absolute QPC-since-boot reading, which is why it printed ~36 s (CE autostarts
at boot).

Confirmed empirically with the project's clang before touching the header: two TUs including one header,
writer sets 42, reader sees `0`; with the type moved out of the unnamed namespace, `42`. **No diagnostic
fires**, not under `-Wall -Wextra`, which is what made this survive.

Blast radius was diagnostics only. The `complete` re-entry guard is written and read inside
`main_recording.cpp`, so it stayed self-consistent and deferred startup never double-ran.

Fixed in `fb2afb1d` by moving the struct out of the unnamed namespace, with a comment saying why it must
stay out. Regression coverage is `tests/test_header_inline_variable_linkage.cpp`: one case anchored on
`main_g_ControllerStartupTiming`, plus a sweep of every first-party header (`common`, `hook`,
`captureengine`, `mediaengine`, `testapp`, `tests`) for an `inline` variable whose type comes from an
unnamed namespace in the same header. Both fail on the parent revision and pass on the fix; the sweep
found no other instance in ~475 headers.

Invariant worth keeping: **a type in an unnamed namespace in a header and an `inline` variable of that
type are mutually exclusive.** Either the type is shared, or the variable is not.

Two smaller things checked and cleared in the same pass, both correct as they stand:

- `[Screenshot] Output color policy: requested=both resolved=HDR AVIF plus BT.709 SDR PNG` followed by a
  single PNG is not a missed HDR write. The swapchain was `SDR-G22-P709`, and `SaveRawScreenshot`
  deliberately writes one PNG for an SDR source. The policy line just prints before the source is known,
  so it reads as a promise of two files. Cosmetic, unfixed.
- `Sharpen: DX12 running reason=cas param=0.000` is the configured `sharpen=cas/0.00/0.30/auto`: CAS at
  minimum sharpness with a 30% blend. `Decide` short-circuits on zero *intensity*, not zero strength, and
  CAS sharpness 0 is the effect's mildest setting rather than an identity pass.

`DisplayTiming` also logged `no screen-change timestamp published yet` at `21:20:42` with
`runtimePresents=0`; that is the loading screen before any present, and the next window published 1219.
Not a fault.
