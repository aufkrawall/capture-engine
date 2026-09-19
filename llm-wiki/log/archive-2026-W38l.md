# llm-wiki Log Archive (2026-09-18)

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

**The `slInit` route shipped dead, and the first hardware run proved it (`20260918_221342`).** Two
independent bugs, neither of which a build or a unit test could catch:

- **Registered 833 ms too late.** It hung off `RegisterAbiSensitiveDynamicHooksOnce`, which waits for
  CE's hook-time generation classification. That ran at 22:13:54.785; the runtime had already
  resolved `sl.common` - which loads from *inside* `slInit` - at 22:13:53.952.
- **Wrong hooking mechanism.** Only a `RegisterDynamicHookFiltered` (GetProcAddress-time) route was
  installed. Alan Wake 2 links `sl.interposer` statically and calls `slInit` through its own import
  table, which only `PatchIATAllModules` reaches.

Fixed by installing from the config-load path immediately after `ce::ngx_ota::PublishPolicy`
(22:13:53.803 in that session, ~150 ms ahead of the `sl.common` load), resolving the generation from
the mapped interposer's own file version via `ce::streamline_api::LiveGenerationFromLoadedInterposer`
- moved out of `main_redirect.cpp`'s anonymous namespace so there is one copy - and patching the IAT
as well as the dynamic route. The monitor loop retries for a late-mapping interposer. Missing the
window remains inert. `tests/test_ngx_ota_policy.cpp` pins both properties at source level, because
that is the only level at which either mistake was visible.

**A correction worth keeping about the module probe.** The same session logs
`EnumProcessModules failed (error=299 ERROR_PARTIAL_COPY)` followed by `d3d12=0`, which the faster
native poller made reachable: CE now enumerates while the target's PEB module list is still being
built, where the old 0.5 s WMI latency had always let the process finish initialising first
(`20260918_162809` reads `d3d12=1` on its first probe). This is **not** an injection regression -
`ce::injection_policy::ShouldInjectAfterGraphicsProbe` deliberately ignores `d3d12Loaded` and injects
immediately either way, so a failed probe never delayed or changed an injection. What it cost was the
diagnostic, plus a log line promising "conservative non-D3D12 injection timing" that describes a
timing path which does not exist. The probe now retries the transient error and the message states
what actually happened.

**Explicitly rejected:** writing NVIDIA's registry, `nvngx_config.txt` or
`nvngx_ota_updates_config.txt`, and driving `nvngx_update.exe` with its undocumented CLI flags
(`-forced_update`, `-force_add_update`, `-bootstrap`, ...). Machine-wide, persistent, affects other
applications - and the in-process CreateProcess route gets the same outcome deterministically.

**Second hardware run (`20260918_223542`), and what it settled.**

- **`ngx_log` works.** The session directory now contains NGX's own
  `nvngx_dlss_310_9_1.log`, `nvngx_dlssd_310_9_1.log` and `nvngx_dlssg_310_9_1.log`.
- **The updater refusal works, but started too late.** Nine `nvngx_update.exe` processes were
  created at 22:35:48, every one parented to the game, while CE published its policy at
  22:35:49.072 and refused from 22:35:49.170 onward. The CreateProcess hook had been installed
  since DllMain - what it lacked was an answer, because `CurrentMode` returned "default" until the
  hook thread's own config load. The injector had published the resolved value at **22:35:42.842**,
  six seconds before the game existed. `CurrentMode` now falls back to that published value, so the
  blind window is closed. The header comment that called the old behaviour "exactly right" was
  wrong and now says so.
- **The `slInit` route installs in time and still did not take effect.** It went in at 22:35:49.085
  with `IAT patched=1`, 164 ms before CE observed the OTA core at 22:35:49.249 - and the core still
  won, with nothing logged either way, because the hook only reported when it actually cleared bits.
  Four causes were indistinguishable: the game called `slInit` before CE, the call missed CE's
  routes, the struct identity check rejected it, or the flags were already clear.

  The hook now logs on **every** entry with the outcome, and the foreign-core observation pairs
  itself with `WasSlInitRouteInstalled()` / `WasSlInitObserved()` and states which of the three
  situations produced the loss. That pairing is the whole point: "installed" and "effective" looked
  identical in a log and cost a session to tell apart.

**Open, and the next run decides it:** if the verdict reads `installed=1, seen through CE=0`, the
game reaches `slInit` before CE is in the process at all, no in-process hook can win, and the only
remaining lever is launching through CE (`--launch`) so injection happens at process creation.

**Also observed:** the `nvngx_update.exe` processes are transient, not permanently stuck - all nine
exited within a minute. The user's original "running and partially hanging" description matches a
burst at game start that lingers visibly and then clears, so the earlier worry that CE's
CreateProcess refusal might deadlock NGX teardown has no evidence behind it.

**Third hardware run (`20260918_224737`, 0.1.6667).**

- **The early-mode resolve works.** `NGX OTA: resolved ngx_ota=off from the injector's published
  config before the hook thread's own config load` at 22:47:47.272, first refusal at 22:47:47.380 -
  290 ms *before* the policy publishes at 22:47:47.670. The window that let nine updaters through
  is closed.
- **`ngx_log` works**, again: NGX's own logs land in the session directory.
- **The slInit diagnostic paid for itself immediately**, reporting in one line what had taken a
  session to guess at:

```
NGX OTA: the OTA core won with ngx_ota=off - slInit route installed=1, slInit seen through CE=0.
The route was installed but the call never came through it
```

**And that verdict was then misread - worth recording, because the wrong conclusion was
"unfixable".** The first reading was that the game reaches `slInit` before CE exists in the process,
reasoning from `sl.interposer.dll` being a static import of the exe. That reasoning conflates two
different events: the static import decides when the **module** is mapped (during process
initialisation), while `slInit` is a function the game calls from its **own startup code**, well
after the entry point. The timestamps settle it:

| Time | Event |
|---|---|
| 22:47:47.137 | CE's `DllMain` - CE is in the process |
| 22:47:47.138 | loader/CreateProcess hooks installed |
| 22:47:47.688 | slInit route installed, from the hook thread's config load |
| 22:47:47.909 | foreign `sl.common` core observed |

CE was present 551 ms before the route went in, and the game's `slInit` landed in that gap - the
same class of mistake as the `CurrentMode` one directly above it, one layer up. The route now
installs from `DllMain` beside the kernel32 loader hooks, which it can do because it needs nothing
else: the generation comes from the mapped interposer's file version and the mode from the
injector's published shared memory, both reads, nothing loaded.

`tests/test_ngx_ota_policy.cpp` pins the call site (DllMain, after the loader hooks, before the
graphics IAT work). The position has been wrong twice for two different reasons and the symptom was
silence both times, so it is asserted rather than trusted.

**Unvalidated on hardware:** `ngx_ota=on`, and the DllMain-time slInit install. The next run's
verdict line decides the latter: `seen through CE=1` means it works, and an unchanged
`installed=1, seen=0` would finally make "unreachable in-process" an earned conclusion rather than
an assumed one.
