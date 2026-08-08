# llm-wiki Log

### 2026-08-08 - NGX export hooks recursed into the core: unfiltered GetProcAddress interception

- **Symptom**: `installed/testapp/dx12_dlss_fg_test.exe` died ~1.5 s after launch with `0xC00000FD` (stack overflow)
  inside `nvapi64_impl.dll`. The dump stack is an unbroken alternation of
  `capture_hook_x64!Hooked_ProcessFeatureRequirements` and `_nvngx!NVSDK_NGX_D3D12_GetFeatureRequirements+0x1c3`.
  Sessions `logs/20260808_143537` and `logs/20260808_143801`.
- **Root cause**: `NVNGXHook::Install` registered the NGX export names with the *unfiltered*
  `IATHook::RegisterDynamicHook`. The feature snippets (`nvngx_dlss.dll`, `nvngx_dlssg.dll`, `nvngx_dlssd.dll`) export
  the same `NVSDK_NGX_*` names, and the core resolves them out of the snippet to dispatch into the feature. CE answered
  that internal lookup with its detour, and the detour forwards through the single per-symbol `nvngx_hook_o*`, which the
  export inline hooks had set to the **core's own trampoline** - so the core body ran again, resolved again, forever.
  The log tell is `GetProcAddress: Intercepting NVSDK_NGX_D3D12_GetFeatureRequirements from nvngx_dlssg.dll`.
- Introduced by 3a02cd0f: before the export inline hooks existed, the shared `original` could not lead back into a
  hooked body. Proven independent of `dlss_fg_preset` by an A/B run (identical crash with the setting removed).
- **Why it needs an app-local `_nvngx.dll`**: the testapp ships its own, so the caller is not under `\system32\` and
  `DetourGetProcAddress`'s system-module caller bypass never fires. A driver-store `nvngx.dll` sits under
  `\System32\DriverStore\`, which the bypass does catch - that is why GTA V Enhanced had not hit this. Any title
  bundling `_nvngx.dll` would.
- **Fix**: `ce::ngx::ShouldInterceptNgxExportLookup` (`hook/common/ngx_module_policy.h`) plus
  `RegisterDynamicHookFiltered` in `nvngx_hook_feature.cpp` - export-name interception now applies to the core provider
  only. This also stops the shared `original` from hiding the snippet's real entry point.
- **Validated on hardware**: `dx12_dlss_fg_test.exe` on build 0.1.5864 now runs indefinitely with zero crash dumps, and
  the same session shows the DLSS FG preset override completing end to end (below).
- **Coverage**: `tests/test_ngx_module_policy.cpp::ExportLookupInterceptionIsLimitedToTheCoreProvider`.

### 2026-08-08 - nv_lod_spread_fix real-DXVK correction: the inject `.data` write was provably too late

- Filter Tester DXVK session `20260808_154051` falsified the original timing premise. The Vulkan layer entered
  `vkCreateInstance` at `15:42:26.778` and `vkCreateDevice` at `15:42:26.825`; the injected hook DLL entered
  `DllMain` only at `15:42:27.119` and changed the setting global at `15:42:27.361`. Its success log therefore said
  only that memory changed, not that driver initialization consumed it. Negative mip bias remained low quality and
  shimmery, while the on-disk `75 05 -> 90 90` reference patch was visibly effective.
- Root fix: apply the exact structurally validated two-byte branch patch to the process-local ICD image. The Vulkan
  layer sweeps immediately after the next `vkCreateInstance` returns and again at device entry, guaranteeing the
  patch precedes device initialization; the inject DLL remains the OpenGL/late-module fallback. No driver file is
  written.
- The resolved config now reaches the Vulkan layer through one byte of `SharedGraphicsConfig`'s existing alignment
  gap. Static offset and size assertions prove the shared layout/signature did not change. Initial publication and
  config updates both copy it, so application profiles remain authoritative.
- Scanner coverage now recognizes an already-patched image only by proving `90 90`, the ON table load, the skip over
  the adjacent OFF load, and the four-byte slot relationship. Source-order coverage pins the post-instance patch
  before `Capture_vkCreateDevice`. The supplied minimal dump independently identifies stock nvoglv32 32.0.16.1088;
  its RPC `0x6BA` exception is unrelated shell drive enumeration, not the quality failure.
- Corrected-build hardware pixel validation remains pending.

### 2026-08-08 - nv_lod_spread_fix original implementation (superseded): in-memory `.data` write

- **Request**: implement the community `nvoglv64.dll` byte patch (FERMI_UNOPT_LOD_SPREAD, which fixes NVIDIA's
  negative-LOD-bias texture filtering quality on Vulkan/OpenGL) as a runtime feature that modifies the driver only in
  memory, with nothing written to disk, and without acting too late to matter.
- **Original rationale**: the reference scripts NOP the `jne` guarding the OFF path, but analysis of the unpatched
  32.0.16.1088 image found one global reader and one writer. The implementation inferred that writing the ON payload
  (`0x37299934`) was timing-equivalent to NOPing the branch and preferred the `.data` page.
- The single writer is `mov [<global>], eax` guarded by `test al,al / je`, right after
  `mov edx, 0x3001ac; call <settings accessor>`: **`0x003001AC` is the DRS id of this setting**, the store only
  happens when that query succeeds, and the accessor is one internal `.data` function pointer shared by ~1000 setting
  reads (not an import, so IAT hooking cannot reach it).
- Validation: replayed the shipped C++ scanner against real images. 1088 and 620.12, x64 and x86, all four resolve to
  the offsets `LOD_SPREAD_PATCH.md` documents. This scanner evidence remained valid; the untested injection-order
  premise did not. The hardware correction immediately above supersedes the `.data` write and re-check anchors.

### 2026-08-08 - dlss_fg_preset added: the FG preset is a DRS key, not an NGX parameter

- **Request**: mirror the SR/RR preset-letter overrides for DLSS frame generation, which NVIDIA introduced in 2026
  (presets A and B so far) and which the driver and NVIDIA Profile Inspector can override.
- **Finding that changed the design**: there is no application-facing NGX parameter for it. Strings and disassembly of
  `nvngx_dlssg.dll` 310.7 (and 310.6) show the create-time parameter set is `DLSSG.UserInterfaceRecompositionEnabled`,
  `MenuDetectionEnabled`, `AsyncCreateEnabled`, the linearized-depth trio and `IndicatorLevel` - no `Preset` name
  anywhere. The preset is read from the driver settings in `DLSSGDRSKeys::ReadValuesFromDRSImpl`, which is why NPI can
  set it: it writes the same DRS profile. Rewriting a parameter the game sets, the SR/RR technique, has nothing to act
  on here.
- **Reverse engineering** (all against a standalone `nvngx_dlssg.dll` 310.7.0.0): the DRS loop at
  `.text+0x1a2bf` walks an 8-entry id table at `.rdata` RVA `0xa0508` = `0x10E41DF6`, `0x104596A1`, `0x104596A2`,
  `0x104596A3`, `0x104D6667`, `0x104C9A99`, `0x10E41DF1`, `0x10308298`. The preset consumer at `.text+0x38223` reads
  `0x10E41DF6` (bit 2 = "Not parsing presets due to private flag overrides") and then `0x10E41DF1`, logging
  `INFO: Preset ID: %d`; `1` -> "Preset A selected, disabling UIR", `2` -> "Preset B selected, enabling UIR",
  `0xFFFFFE`/`0xFFFFFF` are sentinels that land on the same two branches. The read path stamps
  `NVDRS_SETTING_VER1 = 0x13020` and requires `settingLocation == 0`, reading the value from `currentValue.u32Value`
  at struct offset `0x201C`. `NvAPI_DRS_GetSetting` is resolved as function id `0x73BF8338` via `nvapi_QueryInterface`
  and cached at first use.
- **Version floor**: 310.4 (GTA V Enhanced's shipped copy) and the 310.2.1 driver-store copy contain neither the preset
  strings nor `0x10E41DF1`; the repo's testapp runtime is 310.6 and does. So `installed/testapp/dx12_dlss_fg_test.exe`
  can validate this locally - GTA V Enhanced cannot until its FG runtime is newer.
- **Implementation**: `hook/common/ngx_fg_preset_override.{h,cpp}` wraps the resolved `NvAPI_DRS_GetSetting` for that
  one setting id, `config` gains `dlss_fg_preset` (`[DLSS]`/`[Graphics]`, `default|A-Z`, in the default template), and
  the value rides in `SharedGraphicsConfig::dlssFGPreset` - the slot that was retained as padding, so layout and the
  ABI signature are unchanged and an older host publishes a zero that reads as "no override".
- Deliberately **not** done: writing the DRS profile like NPI does (persistent, machine-wide, needs elevation), and
  emulating preset A/B by flipping `DLSSG.UserInterfaceRecompositionEnabled` (that is today's meaning of the letters,
  not the letters themselves, and a driver-side preset would still win).
- **Interception constraints**: nvapi64.dll code bytes are never patched - CE reuses the filtered
  `nvapi_QueryInterface` GetProcAddress/IAT path for the same reason `reflex_limiter.h` documents. `nvngx_dlssg` is a
  Streamline/FG module, which `DetourGetProcAddress` deliberately bypasses, so
  `IATHook::ShouldAllowNgxFrameGenerationPresetDynamicHook` is one narrow exception: that snippet, that export, only
  while a preset is configured. With `default` nothing is armed at all.
- **Coverage**: `tests/test_ngx_fg_preset_override.cpp` (8 tests) pins the ABI mirror, the setting-id and struct-version
  scoping, the current-profile/non-predefined shape of the answer, the module and function-id gating, and the dynamic
  hook exception; `tests/test_config_part2.cpp` and `tests/test_config_override.cpp` cover parsing and per-profile
  resolution.
- **VALIDATED on hardware** 2026-08-08, build 0.1.5864, session `logs/20260808_150232`, after the NGX export-hook
  recursion fix above unblocked it: `configured preset is now 'B'` -> `armed the DRS render-preset override` ->
  `GetProcAddress import patch on nvngx_dlssg.dll installed` -> `wrapping NvAPI_DRS_GetSetting for ...nvngx_dlssg.dll`
  -> `answered NvAPI_DRS_GetSetting(0x10E41DF1) with preset 'B' (value 2, driver status -160, call 1)`. The driver
  status is NvAPI's "setting not found" for a profile that never set it, which is exactly the case the substitution
  exists for, and it is answered once during feature setup. If the `wrapping` line is ever missing, the snippet
  resolved nvapi before CE patched its import.
- **Open question**: whether NVIDIA keeps `0x10E41DF1` as the preset key in later runtimes. The id is not derived at
  runtime; a future snippet that moves it would silently stop honoring the setting (fail-open, no misbehavior).
- **Open question**: no NVIDIA-side readback of the applied preset has been observed. The snippet's own
  `INFO: Preset ID: %d` / `Preset A selected, disabling UIR` lines need NGX logging, which comes from `LogLevel` /
  `EnableConsoleLogging` under the NGXCore key; 310.6 exposes only `__NGX_SHOW_INDICATOR` and
  `__NGX_CUBIN_DISABLE_RESOURCE_CACHE` as environment variables, and the on-screen indicator text carries no preset
  letter. Extending the existing in-process `dlss_indicator_spoof` registry answering to those value names would
  prove the runtime side without touching the machine's registry.
- **Unexplained, seen once**: during preset-A validation the test app froze at ~197 s, main thread parked in
  `dx12_dlss_fg_test!MoveToNextFrame` -> `WaitForSingleObjectEx` on a D3D12 fence, no device-removed in the log. It
  did **not** reproduce: standalone-without-CE ran 376 s, CE-injected-without-the-preset 330 s, and a CE-injected
  preset-A repeat 330 s, all responsive. In the hang dump no CE thread and neither Streamline worker (`sl.pacer`,
  `sl.dlssg`, both parked on condition variables) is blocked inside CE code. Cause not established; treat as an open
  item rather than as an exonerated one, and re-check if it recurs.

### 2026-08-08 - dlss_sr_preset never reached NGX: Streamline resolves it past every snapshot hook

- **Symptom**: `dlss_sr_preset=m` in `[Profile.gta]` left GTA V Enhanced running preset K, with a custom
  `dlss_sr_dll_path` DLL that does contain preset M. Session `logs/20260808_031448`.
- The config side was fine end to end: `Config: Local srPreset is 13`, `Received SRPreset 13 from SHM`, and
  `nvngx_debug.log` full of `NVNGX: Config forced SR Preset to 'M' (via Install)`. What is **absent** from that log is
  the tell: not one `SetUI`, `CreateFeature` or parameter line. Interception never engaged.
- **Root cause**: in a Streamline title nothing CE hooks is ever on the path.
  - `sl.dlss.dll` has no nvngx import and no `NVSDK_NGX_*` strings - only the `DLSS.Hint.Render.Preset.*` parameter
    names. `sl.common.dll` is the real client: it holds ASCII `NVSDK_NGX_D3D12_AllocateParameters` / `CreateFeature` /
    `Init_Ext` (GetProcAddress names) and UTF-16 `nvngx.dll` / `_nvngx.dll` (LoadLibraryW names).
  - `_nvngx.dll` is not in System32 here, so sl.common reads `HKLM\...\NGXCore\FullPath` and loads the driver-store
    `nvngx.dll` directly, then GetProcAddresses every entry point.
  - `sl.common.dll` loaded at **03:15:19.991**. CE's GetProcAddress IAT patch ran at 03:15:09.431 and 03:15:14.455 and
    **never again** - `InitializeGetProcAddressHook` is `PatchIATAllModules`, a snapshot. So sl.common's
    `GetProcAddress` import was never patched and the dynamic-hook table was unreachable.
  - Compounding it: `NVNGXHook::Install` accepted **`sl.dlss.dll`** as "the NGX DLL", logged
    `NVNGX: Found 'sl.dlss.dll' ... IAT patches installed`, and latched `m_Installed` - on a module that exports no NGX
    symbol whatsoever. Every later retry, including after nvngx.dll actually loaded, returned at the latch.
- **Fix**: `InstallNGXExportInlineHooks()` in `nvngx_hook_feature.cpp` inline-hooks the `NVSDK_NGX_*` export bodies in
  nvngx.dll/_nvngx.dll, driven from `NVNGXHook::OnModuleLoaded` in `NotifyHookModuleLoaded` so the patch lands inside
  sl.common's own `LoadLibrary`, before its first `GetProcAddress`. Hooking the body is authoritative: it does not care
  how or when the caller obtained the pointer. Also called ahead of the `m_Installed` latch on the periodic path, and
  the provider search no longer accepts Streamline plugins (`hook/common/ngx_module_policy.h`).
- The captured trampoline **overwrites** `nvngx_hook_o*` unconditionally: an earlier IAT pass may have stored the raw
  export address, which after inline hooking would re-enter our own detour.
- Rejected: inline-hooking `GetProcAddress`. `kernel32!GetProcAddress` is a `4c 8b 04 24` + `48 ff 25` caller-forwarding
  thunk into `kernelbase!GetProcAddressForCaller` (a *different* export from `kernelbase!GetProcAddress`), so full
  coverage needs both, and the `__builtin_return_address(0)` caller filtering in `DetourGetProcAddress` breaks. Also
  rejected: re-running the IAT snapshot on module load - it works in practice but is a load-vs-patch race.
- Downstream parameter machinery needed no change; it was complete and simply never reached.
- **Coverage**: `tests/test_ngx_module_policy.cpp` pins that sl.dlss/sl.common/sl.interposer are never the provider,
  that `nvngx_dlss.dll`/`nvngx_dlssg.dll` (feature DLLs, no NGX API) are rejected, and that the driver-store path and
  the `_nvngx.dll` stub are accepted.
- **Verification**: `--verify` on `0.1.5857`, all stages OK.
- **Stale risk**: high until a real GTA V Enhanced run. Expect `NVNGX: inline-hooked N export(s) in nvngx.dll` in
  `hook_debug.log`, then `SetUI`/`CreateFeature` lines in `nvngx_debug.log`. If the inline-hook line never appears,
  nvngx.dll is being loaded by a path that bypasses `LdrLoadDll`.

### 2026-08-08 - dlss_debug_overlay never worked: one-shot IAT patch could not see nvngx

- **Symptom**: `dlss_debug_overlay=on` in `[Profile.gta]` showed no indicator in GTA V Enhanced even with DLSS SR and
  FG both active. Session `installed/captureengine/logs/20260808_024421`.
- **Root cause**: the spoof was an IAT patch of `advapi32!RegQueryValueExW` applied once from `HookThread`
  (`InitializeAdvapi32Hooks`). The log shows it patching exactly two already-loaded modules at `02:44:42.300`; the
  modules that actually read the value, `nvngx_dlss.dll` / `nvngx_dlssg.dll`, only load at `~02:44:53` and were never
  patched. `PatchIATAllModules` is a snapshot, so nothing loaded afterwards is covered.
- Confirmed empirically rather than assumed: the UTF-16 `ShowDlssIndicator` string exists in `nvngx_dlss.dll` and
  `nvngx_dlssg.dll` only (not `nvngx.dll`, `sl.dlss.dll`, `sl.dlss_g.dll`, `sl.common.dll`), both import
  `ADVAPI32!RegOpenKeyExW`+`RegQueryValueExW`, and `HKLM\...\NGXCore` exists on this machine with only `FullPath` and
  `Installed` - **no `ShowDlssIndicator` value at all**, so the answer has to be fully synthesized.
- **Second, independent bug** in the old detour: it wrote the payload but never set `*lpType` or `*lpcbData`, and it
  bailed out entirely on the `lpData == nullptr` size probe. Even a correctly placed patch would have handed NGX a
  DWORD with an unset type.
- **Fix**: new `hook/common/dlss_indicator_spoof.{h,cpp}` inline-hooks `kernelbase!RegQueryValueExW` and
  `kernelbase!RegGetValueW` once (advapi32 as fallback), with the pure decision logic split out for tests.
  `hook/main_redirect.cpp`'s `HookedRegQueryValueExW`, its globals, and `IATHook::InitializeAdvapi32Hooks` are deleted.
- **Why kernelbase, not advapi32**: `advapi32!RegQueryValueExW` is a 7-byte `48 FF 25 <rel32>` thunk into kernelbase
  followed by `int3` padding - a bad 14-byte-patch target, and invisible to api-set importers. kernelbase's bodies
  start with exactly 14 relocation-free prologue bytes (`48 8b c4 4c 89 48 20 48 89 48 08 53 56 57`), and advapi32's
  thunk lands there anyway. Installed via `InstallPublished` so the trampoline is live before the patch is.
- Rejected: re-running the IAT patch from the periodic module scan. It would have worked in practice but reintroduces
  a load-vs-patch race, which the project rules forbid as a fix.
- **Coverage**: `tests/test_dlss_indicator_spoof.cpp` - mode parsing, passthrough, non-matching value names, the
  complete DWORD answer, size probe, `ERROR_MORE_DATA`, `RRF_RT_*` handling, plus config-layer tests that a
  `[Profile.*]` value beats the global one.
- **Verification**: `--verify` on `0.1.5856` - build, unit tests, Python self-tests, lint (clang-tidy 0 warnings),
  sanitizer cadence, privacy scan all OK.
- **Stale risk**: high until a real GTA V Enhanced run confirms the indicator appears. Look for
  `DLSS indicator: dlss_debug_overlay=on armed` and then `DLSS indicator: answered RegQueryValueExW(...)` in
  `hook_debug.log`; if the second line is missing, a newer NGX build reads the value some other way.

### 2026-08-08 - Repo recreated to purge the scrubbed objects; runner workFolder is load-bearing

- The dangling pre-scrub commits could not be removed by a force-push (see the previous
  entry), so the repository was **renamed to `capture-engine-dev2` (kept private) and a
  fresh `capture-engine` created** with only the clean history. Chosen over delete+recreate
  because it is reversible and loses nothing: the archive keeps the old run history,
  creation date and the `v0.1.5294` release. Verified afterwards: all four leaked SHAs
  answer **HTTP 422** on the new repo and the contents endpoint 404s, i.e. the objects were
  never there rather than merely unreferenced - a stronger guarantee than a Support gc.
  `capture-engine-wip` (a separate older copy) does not contain them; no public repo does.
- Restored by hand after the recreate: 1897 commits + the `v0.1.5294` tag,
  `CE_TOOLCHAIN_ROOT`, `default_workflow_permissions=read`
  (`can_approve_pull_request_reviews=false`), and the runner registration.
- **`--work` is load-bearing when re-registering the runner.** The original registration
  used an explicit `workFolder` of `C:\...\build\runner-work`; re-registering with
  `--unattended` and no `--work` silently used config.cmd's default `_work`, which sits
  under the runner directory and put the workspace at **82** characters instead of **73**.
  Run 31226827240 then died seven minutes in, after a fully successful closure and FFmpeg
  build, while "Linking Hook DLL x64":
  `EXCEPTION: [WinError 206] The filename or extension is too long`. The link step passes
  hundreds of absolute object paths on one command line and nine characters were enough to
  exceed Windows' 32767-character command-line limit. Note this is the command-line limit,
  a *different* limit from the 260-char MAX_PATH that the doxygen man pages hit - the same
  path-depth theme, two distinct mechanisms.
- The runner's work folder is machine state, so no repository test can pin it.
  `release-stable.yml` now preflights the workspace length as its **first** step (limit 76;
  measured 73 passes, 82 fails) and names the fix. Seconds instead of seven minutes, with a
  message that mentions the runner rather than a filename.
- Gotcha inside that guard: a PowerShell here-string terminator must sit at column 0, which
  dedents out of a YAML block scalar and makes the workflow unparseable. Build the message
  with `-join` instead. The first attempt did exactly this and broke the file.
- Cleanup of the stray 4.4 GB `_work` tree followed the junction rule that once destroyed
  the dev toolchain: enumerate reparse points, `[System.IO.Directory]::Delete(path, $false)`
  each one, assert none remain, and only then recurse. Both junction targets
  (`external`, `build\msys64`) were verified unchanged afterwards.

### 2026-08-07 - v0.1.5294 released; Actions run logs were a second, larger leak surface

- `v0.1.5294` published by run 31218094687 (success, `step.external_preparation` 1111 s, so
  compiled in-job), log auto-deleted (404), published assets carry no user or host name.
  Replaces the deleted `v0.1.5293`. Next version: `0.1.5295`.
- **Checking "are the dangling SHAs the only leak?" found two further surfaces.** They were
  worse than the git objects, because no SHA was needed to reach them:
  - **Six retained `release-stable` failure logs.** `release-log-cleanup.yml` deliberately
    keeps a failed run's log (`if: conclusion == 'success'`) as the only diagnostic material,
    and flags that it "still needs a manual delete once investigated". That manual step had
    never been performed, so six had accumulated - each exposing the **machine name**, which
    is the entire reason the cleanup workflow exists, and one also the mangled user path.
  - **Two logs from a `debug-token` workflow that no longer exists**, both marked *success*.
    The cleanup matches `workflows: ["release-stable"]` by name, so any other workflow's logs
    are unprotected by construction, and with the workflow file deleted nothing would ever
    have cleaned them.
- All eight deleted, each verified `404` with the same fail-closed re-check the cleanup uses.
  A sweep of **all 36 runs** with a broad pattern (the name in any spelling, which also
  catches the host name) now reports **0 leaking logs**; 24 logs remain retained and clean.
  Zero workflow artifacts.
- **Design gaps this exposed, still open** (deliberately not changed unilaterally):
  the "manual delete once investigated" step will be forgotten again - it already was, six
  times - so failed logs want an age-based auto-delete or an issue opened by the flag step;
  and the cleanup's single hard-coded workflow name means a new or deleted workflow is
  silently outside its scope.
- Remaining exposure before the repo can go public is **only** the dangling git objects from
  the history scrub: still fetchable by SHA, and the SHA is advertised in the Actions run
  list, so it needs no guessing. Needs a GitHub Support garbage-collection request, or a
  repository delete-and-recreate (which also wipes the run history advertising the SHAs).

### 2026-08-07 - nv-codec-headers pinned and given a fallback source (git.videolan.org outage)

- Release run 31215691866 (0.1.5294) built the **entire** dependency closure, including aom
  with the fixed key and no import error, then died on
  `git clone https://git.videolan.org/git/ffmpeg/nv-codec-headers.git`:
  `Failed to connect to git.videolan.org:443 after 21273 ms`. The host was still down
  minutes later, so this was an outage, not a blip.
- **Two real gaps, both fixed together:**
  - `git_clone` had **no retry at all**, while `download_file` has had bounded retry since
    `0208d09b`. Same lesson - a run that has already compiled for minutes must not be lost
    to one unreachable host - applied to a path that had been missed.
  - `nv-codec-headers` was cloned from **master, unpinned** (the `llm-wiki`/hand-off
    follow-up), so a fresh build took whatever upstream had merged that day.
- Fix: `FFNVCODEC_SOURCE_REF` pins the exact commit previous releases were already built
  against (`eddcea9e...`, "Bump for (in-dev) 13.1.15.1"), so pinning changes nothing about
  the product and only removes the non-determinism. `FFNVCODEC_URLS` adds the FFmpeg
  project's own GitHub mirror as a second source, and `git_clone` now tries each source
  twice, deleting any partial tree between attempts so a half-clone is never mistaken for
  a usable one.
- **The fallback is only sound because of the pin.** Both hosts were verified to serve that
  commit with the identical tree hash `2fd41cd5544091f6d0d27d0771a9cb7b838fd554`, so which
  host answers cannot change what is built. Without a pin, a second host would silently be
  a second source of truth.
- The ref feeds `ffmpeg_build_configuration_fingerprint()` for the same reason as
  `FFMPEG_SOURCE_REF`: `--skip-updates` builds return early when prebuilt DLLs exist, before
  the source is consulted, so a pin change outside the fingerprint would keep shipping
  FFmpeg built against the previous NVENC headers.
- Verified live while git.videolan.org was actually down: the real `git_clone` logged
  `Clone of ffnvcodec from git.videolan.org failed; trying the next source`, cloned from
  github.com, and checked out the pinned commit.

### 2026-08-07 - History scrubbed a third time; a force-push does NOT purge GitHub

- Documenting the log-scrub fix put the maintainer's real user name into four tracked files
  as `C__Users_<name>_Programme_...`, and it reached `origin/main` in three commits plus
  the `v0.1.5293` tag tree. Commit authorship is deliberately clean
  (`aufkrawall <...@users.noreply.github.com>`), so this would have been a genuinely new
  exposure rather than something already visible.
- **Why nothing caught it:** `test_no_developer_user_paths_in_tracked_files` matched only
  path-shaped occurrences, the same blind spot the binary scrub had (fixed `a9590837`) and
  the log scrub had (fixed `ebf962e0`). Third instance of one defect in three places.
  `MANGLED_USER_RE` now covers the underscore-mangled identifier form.
- Scrub performed with `git filter-repo --replace-text --replace-message` (the name was in
  a commit *message* too, which `--replace-text` alone would have missed), replacing
  `C__Users_<name>_` with `C__Users_TestUser_`. `TestUser` rather than `<developer>` so the
  rewritten historical blobs still satisfy the gate's allowlist and the detector's regex,
  which rejects `<` and `>`. Minimal rewrite: `18273781` and earlier kept their SHAs.
- **The important finding: a force-push does not remove anything from GitHub.** After
  force-pushing the rewritten `main` *and* deleting the `v0.1.5293` release and tag, all
  three old commits were **still fetchable by SHA**, and the leaked comment was still
  readable through the contents API at the old ref. Unreferenced objects stay served until
  GitHub garbage-collects, which is not automatic.
  - The remedy is to ask GitHub Support to garbage-collect the repository. This repo has
    **0 forks and network_count 0**, which is what makes that possible - objects in a fork
    network cannot be removed.
  - Consequence: **the repository must stay private until that purge is confirmed.** While
    private, the dangling objects need repo access to read, so they are contained; going
    public would expose them to anyone holding a SHA.
- Local hygiene: dropped a stale `refs/remotes/scrubbed/main` from the 2026-08-04 scrub
  (no configured remote, and verified clean) and the rewritten backup branch. A bundle of
  the pre-scrub state is kept outside the worktree in
  `build/ce-pre-scrub-backup/`, deliberately not in the repo.
- `v0.1.5293` was deleted and replaced by `0.1.5294` so the published release points at a
  commit that still exists.
- Runner-stop correction: `run.cmd` is a wrapper that **restarts** `Runner.Listener` when it
  dies, so killing the listener alone does not stop the runner - it silently comes back.
  Kill the `cmd.exe` running `run.cmd` first, then the listener, and confirm the GitHub-side
  status reports `offline`.

### 2026-08-07 - Stable release v0.1.5293 published, built entirely in-job

- Run 31210650635 succeeded in 24 min. First stable release that satisfies the original
  goal: **every shipped binary was compiled by the run that published it**, so a future
  artifact attestation would cover what it claims to. Author `github-actions[bot]`,
  4 assets, not draft.
- Evidence the closure was really built in-job rather than reused:
  `step.external_preparation` took **1037 s** (local from-scratch equivalent 956 s). If that
  step ever returns in seconds, the closure is being reused and the release is not
  attestable. The run log is auto-deleted, so read `latest_summary.txt` from the release
  assets instead of grepping the log.
- Log deletion verified: `GET /actions/runs/31210650635/logs` -> **404**.
- Privacy audited independently on the published assets: no user name, no host name. Paths
  read `C:\Users\<developer>\Programme\build\runner-work\...` - user component redacted,
  directory layout surviving, which is the deliberately deferred item (needs a project-root
  `-ffile-prefix-map`), not a regression.
- Artifact attestation step was **skipped**: the repository is private and GitHub
  Free/Pro/Team cannot attest private repositories. The property that makes attestation
  meaningful now holds, so it becomes real when the repo goes public.
- Five attempts failed before this one, each on a different runner-only fault. What made
  this one pass first time was rehearsing locally first - see build.py.md "Rehearsing the
  release closure locally".
