# llm-wiki Log Archive (2026-09-19)

### 2026-09-19 - The sl.* override lost to a 400 ms policy gap, not to injection timing

Asked to think hard about whether the `sl.common` loss is really unfixable with existing hook
infrastructure. It is not, and the reasoning that called it unfixable was wrong in the same way
twice before in this file.

**What was claimed:** Alan Wake 2 statically imports `sl.interposer`, so `slInit` runs before CE
exists and nothing injected afterwards can reach it.

**What the binaries actually say:** `sl.interposer.dll` imports **only KERNEL32** - no `sl.common`,
no NGX. So `sl.common` is loaded with a single dynamic `LoadLibrary` from the interposer, which is
precisely what CE's loader hook covers. Same for `_nvngx.dll`: nothing in the chain is a static
import, so "statically imports sl.interposer" says when the *interposer* maps, and nothing about
when the core loads.

**Where it actually loses.** CE's LoadLibrary hooks install in `DllMain`, but every branch of
`GetRedirectedPath` read `g_pLocalConfig`, which the **hook thread** fills from config.ini ~400 ms
later (19:48:28.860 vs ~19:48:29.25 in `20260919_194818`). A load in that window reaches CE's hook
and is answered "no override" **because the policy is missing, not because it says no** - identical
to the `ngx_ota` mode bug fixed earlier the same day, one layer over. CE's own Cyberpunk note
already recorded the shape without naming it: the core arrived "463 ms before CE's loader redirect
**was armed**".

Losing the core is not one plugin: `g_ForeignStreamlineCoreObserved` latches and every later `sl.*`
redirect is refused, which is why `20260919_194818` shows five OTA-store plugins loading at
19:48:30.11-.15 - **observed by CE, 1.25 s after DllMain** - and refused anyway.

**The fix, with the infrastructure already present.** `streamlineDllPath` and the three
`dlss*DllPath`s have always been in `SharedGraphicsConfig`, published by the injector before the
game starts - the same channel `ngxOtaMode` uses. New `hook/common/published_graphics_config.*`
reads them once in `DllMain`; `main_redirect.cpp` gained `Configured*DllPath()` accessors that
prefer the hook thread's config and fall back to the published copy. `NeedsLoaderRedirectionHook`
and the model-store branch stop being gated on `g_pLocalConfig` too.

**One trap worth recording.** `EnsureLocalConfigAllocated()` runs in `DllMain` and leaves a
default-constructed `AppConfig`, so `g_pLocalConfig != nullptr` proves allocation, not content. An
accessor testing only the pointer takes the local branch **always**, reads empty strings, and makes
the early path dead code - the exact silent-inertness the slInit route shipped with twice. Hence a
new explicit `g_LocalConfigLoaded`, set immediately after `LoadConfig`, and a test that pins it.

**Worth knowing:** the substitution is real, not cosmetic. The OTA store serves sl.common
**2.14.0-rc2** (`OriginalFilename: SHA: 614ea534a v2.14.0-rc2`) while the configured `npi\sl` set is
**2.14.1**. The driver was quietly swapping a release candidate in.

**Unvalidated.** Whether Alan Wake 2's `sl.common` load falls inside that 400 ms window or before
CE's `DllMain` is still unmeasured - the load was never observed, only found by the already-loaded
scan. The next run decides it: `Loader redirect: armed from the injector's published config in
DllMain` should appear, and then either a `Redirecting ... (NGX model sl_common_0)` line (fixed) or
the same foreign-core verdict (the load genuinely precedes CE, and only launching through CE is
left).

### 2026-09-19 - ngx_ota=off had the weaker mechanism as its primary one

Asked to make `ngx_ota=off` reliably stop the driver spawning NGX updaters. Four gaps, found by reading
`_nvngx.dll` rather than by another session.

**What the module actually does** (driver r616.92, `nv_dispi.inf_amd64_b20cc8aeaed64fc2\_nvngx.dll`): it imports
exactly `CreateProcessA`, `CreateProcessW` and `GetEnvironmentStringsW` from kernel32 - no `ShellExecute*`, no
`CreateProcessAsUser*`, no `WinExec` - so the two hooks CE already has are a complete set for the in-process
launch path. `nvngx.dll` imports the same two. Its OTA decision points are the environment
(`__NGX_DISABLE_UPDATER`), `SOFTWARE\NVIDIA Corporation\Global\NGXCore` in the registry (which on this machine
carries no `EnableOTA` value at all), and an `.EnableOTA` key in `nvngx_config.txt` /
`nvngx_ota_updates_config.txt`. The last two remain **rejected**, unchanged: machine-wide and persistent.

**1. The primary mechanism was being applied ~550 ms too late.** The environment variable is the only thing that
stops NGX *attempting* a launch ("OTA disabled by environment. Using embedded snippet only"); refusing the
`CreateProcess` call is a backstop that happens after the core has already decided to run the updater. It was
published from the hook thread's config load. In `20260918_224737` that was 22:47:47.670 against a DllMain at
22:47:47.137 - the same class of mistake as the two before it in this file, one layer further out.
`ce::ngx_ota::ApplyEarlyPolicyFromPublishedConfig()` now runs in DllMain beside the loader hooks, using the mode
the injector published in shared memory. It also runs in **launcher** processes, which never call `PublishPolicy`
at all, so a game CE launches inherits the variable before CE's DLL is in it.

**2. `HookedCreateProcessW` decided on a truncated string.** It converted into `char[MAX_PATH]` and matched on
that. `WideCharToMultiByte` writes *nothing* when the destination is too small, so a command line over 260
characters left the buffer empty, `IsNgxUpdaterImage("")` answered false, and the updater went through
unrecognized - silence, not an error. The policy is now templated over the character type and the W hook decides
on the caller's own wide string before any conversion. The narrow buffer for the injection whitelist grew to 2048
for the same reason.

**3. A late-loaded `_nvngx.dll` kept its real CreateProcess imports.** CE's hook is an IAT *snapshot*: DllMain,
then once more on the hook thread. `PatchLoadLibraryIatForLateLoadedModule` repaired only the four loader imports,
and only when `NeedsLoaderRedirectionHook()` - i.e. when a `dlss_*_dll_path`/`streamline_dll_path` is configured.
A plain `ngx_ota=off` profile with no path overrides therefore had **no** late-module coverage, and a title that
initialises DLSS from an in-game toggle maps `_nvngx.dll` long after both passes. New
`PatchProcessCreationIatForLateLoadedModule`, ungated, called from `NotifyHookModuleLoaded`. It also repairs
child-process injection for late-mapped modules, which had the same hole.

**4. The early resolve had a one-shot race.** `g_EarlyModeResolved.exchange(true)` latched *before* the read, so a
second thread arriving during it was told "default" - the one answer that lets an updater through. It also cached
that answer when no CE host had published yet. The read now reports whether a host actually answered, only that
is cached, and a caller finding the read in flight repeats it rather than settling.

**Not evidence of a bug:** nine `nvngx_update.exe` ran today at 19:12:58 (Talos, session `20260919_190127`).
That session has `ngx_ota=default` in `config.ini` and no `NGX OTA: ngx_ota=` line in `hook_debug.log` - the
feature was simply off. Checking the configured mode before reading updater activity as a failure is the cheap
step that was missing.

**First run (`20260919_193954`) was `ngx_ota=on`, and settled three things anyway.**
`config.ini` read `ngx_ota=on` (written 19:38:49, before the 19:39:54 session), CE logged
`ngx_ota=on - cleared __NGX_DISABLE_UPDATER (DllMain)` at 19:40:42.647, and nine updaters ran from
19:40:43. That is `on` behaving exactly as defined, not a failure of `off`.

- **The DllMain environment write works**, in the `on` direction, with the new `(DllMain)` phase tag -
  and `ngx_ota=on` was previously unvalidated on hardware.
- **The late-module CreateProcess patch found real modules that were escaping CE entirely**:
  `nvgpucomp64.dll`, `nvngx_dlssg.dll`, `nvapi64.dll`, `nvcuda64.dll`, `nvdxgdmal64.dll`,
  `nvdiagclt64.dll` and five OTA-store `sl_*` plugins, all importing `CreateProcessW`, none of them
  covered by the DllMain or hook-thread snapshot.
- **`_nvngx.dll` was NOT late here.** It mapped at `00007FFC37560000` before CE's DllMain pass and had
  its `CreateProcessW` slot patched at 19:40:42.645 - 0.4 s before the first updater launch. In this
  title the snapshot already reached it; the late-load repair is for titles that initialise DLSS later.

**And it exposed a diagnostics hole worth more than the run.** CE logged *nothing* about those nine
launches, because only the refusal path wrote a line. "CE never saw the launch", "CE saw it and the
mode permits it" and "CE refused it" were indistinguishable in the log - the same
installed-vs-effective mistake as the slInit route, one subsystem over. `NoteUpdaterLaunchAllowed`
now reports every observed updater launch with the mode responsible.

**Second run (`20260919_194456`), also `ngx_ota=on`, and the new diagnostic settled the open
question anyway.** CE reported eight `saw an NGX updater launch ... and let it through - ngx_ota=on`
lines between 19:45:13.383 and .686. NVIDIA's own side wrote **nine** `nvngx_update*.log` files, all
with `Log begin` at 19:45:13-14: one `-feature dlss`, five `dlssg`, three `dlssd` - per-feature
retries, not nine distinct decisions.

So the burst is nine and CE's CreateProcess hook is on the path for it. **That is the answer to "can
`off` actually stop these": yes, every one of them is reachable in-process**, because `_nvngx.dll`
maps before CE's DllMain pass in this title and its `CreateProcessW` slot is CE's 0.4 s before the
first launch.

The eight-of-nine gap was **CE's own rate limit**, `index < 8`, against a burst of exactly 9 - the
threshold clipped the last one and made "did CE see all of them?" unanswerable from the log, which is
the single question the diagnostic was added for. Now `kFullyLoggedLaunches = 32`, which clears the
observed burst with room for the extra retries a refusal can provoke. Picking a rate limit below a
known burst size is the failure mode to remember here.

**`ngx_ota=off` VALIDATED (`20260919_194818`, 0.1.6697): 15 attempts, 15 refused, zero updater
processes created.** No `nvngx_update*.log` was written after 19:47, nothing was left running, the game
ran normally, and `nvngx_dlss`/`dlssd`/`dlssg` all loaded from the configured `npi\sl` folder instead of
the OTA store - which was the point of the setting.

**And it inverted the model this entry was built on.** The prediction above was that a good `off` run
shows the environment variable winning and therefore *no* refusal lines, with refusals meaning a
degraded fallback. The opposite is true for this class of title:

| Time | Event |
|---|---|
| 19:48:28.956 / .973 | `_nvngx.dll` at `00007FFC36330000` gets its `CreateProcessA`/`W` slots patched **in the DllMain pass** |
| 19:48:28.974 | CE publishes `__NGX_DISABLE_UPDATER` |
| 19:48:29.270 | first launch attempt, refused |

`_nvngx.dll` appearing in the *DllMain* IAT pass rather than the late-load path means it was already
mapped when CE arrived - so it had initialised, and read its environment, before CE existed in the
process. **No injection speed fixes that**, exactly like the `slInit` case one layer up, and for the
same structural reason (Alan Wake 2 statically imports `sl.interposer`). The environment variable is
the optimization; the CreateProcess refusal is the mechanism. Refusal lines are the healthy signal,
not a warning.

The new threshold earned itself here too: 15 refusals, all logged, where the old `index < 8` would
have shown 8 and hidden the rest.

**Still unfixed, and unchanged:** `sl.common` again resolved to
`C:\ProgramData\NVIDIA\NGX\models\sl_common_0\...\1B0_E658703.dll`, so every `sl.*` redirect was
refused and the verdict still reads `slInit route installed=1, slInit seen through CE=0`. Refusing the
updater stops *new* downloads; it does nothing about plugins already in the store. Launching the game
through CE remains the only lever for that half.

### 2026-09-19 - Streamline regular development build instructions (eliminate verify/package overkill)

- **Problem:** Regular development instructions in `AGENTS.md` and `llm-wiki/build.py.md` mandated the full
  `--verify` gate for changes across capture, CFR, FG, and audio paths, and omitted `--skip-package` from
  the ordinary incremental gate. On warm caches, `--verify` took ~201 s (~3.35 minutes) with ~67 s spent
  compressing release 7z archives (`captureengine.7z`, `testapps.7z`, `ffmpeg-corresponding-source.7z`),
  ~42 s on ASan/UBSan child validation, ~28 s on static analysis preflight, and ~31 s on clang-tidy/tool self-tests.
  Routine dev builds were severely bottlenecked by pre-release checks.
- **Correction:**
  - Standard per-change gate for regular development across ALL code areas (including capture, CFR, FG, and audio):
    `python build.py --incremental --skip-package --run-tests --skip-updates --concise` (~25–45 s).
  - `--skip-package` is made standard for ordinary development commits to eliminate redundant 7z archive creation.
  - Linting (`--no-build --lint`), sanitizers (`--sanitize`), and complete verification (`--verify`) are reserved
    for pre-release validation, release candidates, or explicit on-demand checks.

### 2026-09-19 - The Steam-overlay break was one unreachable precondition, not a race worth retrying

Session `20260919_183858`, with the named quiesce reason from the previous entry in place. It paid
for itself immediately:

```
DeepHook: Refusing live patch at 00007FFCA0E4953E — quiesce=unstable-thread-snapshot
  ownershipChanged=0 VirtualProtectError=0 retryable=1
Present body hook attempt 1/4 ... 2/4 ... 3/4 ... 4/4 refused (unstable-thread-snapshot)
```

All four attempts, same reason, and all of them between 18:39:24.863 and .865 — **2 milliseconds**,
entirely inside NvPresent64's worker-creation burst. The retry was the right idea with the wrong
shape: retrying harder inside the burst cannot help.

**What the stability requirement is actually worth.** `ThreadQuiescence` required the thread walk to
reach a pass that discovers NO new threads. The property the patch needs is narrower and is checked
separately: `IsRangeSafe()` — no SUSPENDED thread's instruction pointer is inside the bytes about to
change. The stronger requirement does not close the residual hole either: a thread created after the
final walk is unsuspended in both modes. Under Smooth Motion at D3D init it is simply unreachable.

**Fixed.** `UnstableSnapshotPolicy::kAcceptSuspendedSet` keeps `IsRangeSafe()` and drops the
unreachable no-new-threads guarantee. It is opt-in per transaction; every existing caller keeps
`kRefuse`. Only the LAST of the four Present-body attempts uses it, so the strict path is still
preferred and the relaxation is a last resort before losing the below-the-chain view for the session.
A patch that lands this way says so in the log.

**Still conditional.** If the range genuinely is not safe the patch is still refused, CE stays above
Steam, and the bypass still drops Steam's overlay. That remaining case needs CE's detour to survive
Steam calling back through `vtable[8]` — the crash the bypass was added for.

Gate: `--verify` 0.1.6692. Hardware run pending.
