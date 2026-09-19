# llm-wiki Log

### 2026-09-19 - Measuring the right rate was not the same as showing it

Session `20260919_180154`, driver vsync forced to 144. The classifier from the entry below was
already correct - the cadence window read `application=144.0 fps output=288.0 fps generating=1
multiplier=2`, exactly the game's rate and exactly twice it - and the overlay still displayed 288.

The overlay's frame rate comes from `dxgi_shared_g_DXGIPerfMetrics`, which `UpdateDXGIPresentMetrics
AndPublish` advances once per Present. On a present interposer's private output chain that is once
per OUTPUT frame. DX12 never had the problem because its metric is fed by the app-facing swapchain
wrapper, which is 1x by construction; DX11 has no app-facing view at all under an interposer, so the
metric was counting the interposer's submissions.

Ruled out first: that the 2x was CE summing the interposer's TWO private output chains. Only one of
them (`000001F15890C8E0`) is ever presented - 1814 overlay draws on it, zero on the other - so the
2x is real generation, not double counting.

**Fixed.** The source is resolved at the top of both present entries
(`ClassifyPresentInterposerPresentSource`), before anything measures a rate from the present, and
exactly once - reading it consumes the submission counter, so `HandleDX11ProcessFrame` now consults
the stored verdict instead of classifying again. The metric skips generated presents; the output
stream is still counted in full by `NotePresentInterposerOutputPresent`, so the FG row keeps both
rates.

Gate: `--verify` 0.1.6681. Hardware run pending.


### 2026-09-19 - The overlay's DX11 Smooth Motion fps was the interposer's output rate

Follow-up to the crash entry below: with Witcher 3 surviving, two things were visible for the first
time. Both were the same bug wearing two hats - CE was treating the interposer's OUTPUT presents as
the game's frames.

**The flicker.** `DrawDX11Overlay` had a 500 us "exact duplicate" suppressor that returned early for
any present within that window on the same swapchain and buffer, on the guess that "Smooth Motion can
trigger paired Present callbacks for the same frame". On the interposer's private output chain that
sub-millisecond partner is the generated frame, not a duplicate callback. Session `20260919_160555`:
2453 of 6962 presents (35%) skipped, every one reaching the screen with no overlay. The chain is
presented once per displayed frame, real and generated alike, so the suppressor no longer runs there.

**The fps.** Measured from that session: 6962 presents over 40.8 s, in **3231 groups of exactly two**,
median 432 us within a group and 6590 us between them. ~85 application fps, ~171 output. The overlay
showed ~171, and the FG row doubled it again (`base_fps=377 output_fps=754` at the startup transient).

`RecordPresentForNvidiaSmoothMotion` recorded a constant `1` for every present, with the comment "DX11
and Vulkan do not have the DX12 command-list classifier". So `realFrames` was the whole population and
`cachedBaseFPS` was the output rate. The two-stream `CadenceTracker` could not help either: its
application stream is fed by the app-facing swapchain wrapper, and under Smooth Motion in DX11 CE has
no app-facing view at all - NvPresent64 intercepts the create above CE, so both chains CE sees are its
private ones.

**The second stream was available all along, on the game's own context.** CE wraps the game's
`ID3D11DeviceContext`. A generated frame is produced entirely inside the interposer, so the
application's context is idle across it; one or more submissions since the previous present means this
present carries an application frame. Zero-versus-nonzero - no threshold, no gap window, no timing, so
a light application frame still counts and driver metering changes cannot move it. Counting is off
until an interposer chain is registered, so the ordinary draw path pays one relaxed atomic load.
CE's overlay and the interpolation both run on the interposer's device under Smooth Motion, so
neither can inflate the count.

Classified presents now feed the same `CadenceTracker` DX12 uses: every private-chain present is an
output present, the application-sourced subset is an application present, and the ratio is the
generation factor. `UpdateMetrics` compares against zero instead of a work threshold when the caller
already resolved the source. `DetourPresent: Present interposer output cadence window` now appears in
DX11 sessions and is the line to read.

**Hardware run pending** for the fps and flicker fixes. Gate: `--verify` 0.1.6680.


### 2026-09-19 - Witcher 3 + Smooth Motion: the crash CE could not dump, and why CE caused it

Session `20260919_154534`, DX11, RTX 5070. The game died ~7 s in, twice, with CE inject + overlay and
Smooth Motion on; without CE it does not. No `.dmp` in the session directory.

**Why there was no dump.** Exit code `0xC0000409`. That is `__fastfail`, dispatched by the kernel with
`FirstChance = FALSE`, so the VEH, the SEH chain and the unhandled filter CE installs are all skipped -
CE's crash handler is structurally unable to run, and `crash.log` was never even created. CE's "last
resort" for exactly this case wrote WER LocalDumps values under **HKCU**, which WER never reads; the
key for `witcher3.exe` named the session directory and WER wrote to `%LOCALAPPDATA%\CrashDumps`
anyway. The dump existed the whole time, 110 MB, in the one place CE never looked. ~50 stale HKCU
subkeys had accumulated there since June, each carrying the user's own paths.

**What the dump said.** `NvPresent64.dll+0x1b6fd1` is `int 29h` preceded by `mov ecx, 7` -
`__fastfail(FAST_FAIL_FATAL_APP_EXIT)` out of the UCRT's `abort()`, called from NvPresent64's own
`std::terminate`. An unhandled C++ exception inside the interposer, on the game's render thread, three
frames below the game's call into it. No CE frames on that stack; CE's threads were idle. Both runs
identical to the byte.

**What CE did wrong.** `NotePresentInterposerPrivateSwapchainCreate` correctly classified NvPresent64's
two private output chains and logged "no queue observed, so the overlay stays on the application-facing
chain" - but the code that enforces that lived inside `if (ctx.api == APIType::D3D12)` in
`ExecutePresentCore`. The DX11 branch never saw it. So CE composited onto NvPresent64's private output
chain, with NvPresent64's own D3D11 device (`0000022DD76731C0`, not the game's `0000022DB68BF0D0`), and
kept a retained RTV on its back buffer across Presents. That chain is hooked `presentOnly`, so the
interposer's own `ResizeBuffers` is invisible to CE and nothing ever dropped the pin. There was also a
path that adopted whatever RTV was bound at Present entry whenever NvPresent64 was loaded - on that
chain, one of the interpolator's intermediates.

Under Smooth Motion in DX11 CE has **no application-facing view at all**: NvPresent64 intercepts the
create above CE and both creates CE sees are its own. The wiki listed that as "not observed, not ruled
out"; it is now observed. So passing the private chain through untouched would mean no overlay, which is
not acceptable - the route had to stay a composite, just a safe one.

**Fixed:**
- `ce::overlay_compat::ResolvePresentInterposerCompositeRoute` answers the route for **every** API, in
  `ExecutePresentCore` before the D3D12 branch. D3D12 keeps the queue rule unchanged; DX11/DX10 get
  `kOutputChainTransientBackbuffer`, because there is no queue and the constraint is retention, not
  submission.
- DX11 on an interposer's private chain: the back-buffer RTV is created and released inside the one
  Present, any retained RTV from a previous chain is dropped on entry, and the bound render target is
  never adopted as the overlay target.
- Dump capture: the inert HKCU LocalDumps writes are gone and the leftovers are purged once from the
  controller; `ce::wer_dump_adoption` claims WerFault's dump into the session directory on the
  injector's normal poll ticks; the tracked-exit log now names the exit-code class; and
  `SEM_NOGPFAULTERRORBOX` is no longer set in CE processes or injected games, because it makes the
  default unhandled filter terminate without invoking WER at all.

Tests: `CrashDumpPolicyTest.*` (fail-fast classification, WER file naming, adoption gating and window,
CE-written-subkey recognition) and `PresentInterposerCompositeRouteTest.*` (route per API, bound-RTV
refusal). The call-site wiring itself is not unit-testable without a real D3D11 present.

**Hardware run pending.** Not validated: whether the transient route actually stops NvPresent64
terminating. Known remaining gap: the DX11 overlay device/context are cached from the first chain CE
draws on, while Smooth Motion creates two private chains on two different devices.


### 2026-09-19 - Two short recordings produced nothing, and the overlay said "Finalizing"

Session `20260918_235601`, recordings r0003/r0004. The user asked whether finalization had hung or
only the desktop overlay. Only the overlay - and the real finding was worse: neither recording
captured anything and neither saved a file.

**Recording start is asynchronous and slow.** The media process is spawned on the hotkey and must
load mediaengine, run the render->loopback A/V probe, init the engine and route capture before it
polls its first command. Measured that session: r0003 hotkey 01:09:30.613, first command poll
01:09:34.223 (+3.61 s); r0004 +3.59 s. Both were stopped after ~2.8 s, i.e. inside the startup
window. The media `StopRecording` handler deliberately consumes the queued `cmdStartRecording` and
exits, so `StartRecording` never ran - no `[RECORDING FINALIZATION]` line, no `status=` in either
manifest, no output. r0001 (WGC) took 6.66 s from hotkey to `isRecording=1`.

**Why the overlay stuck.** `CompleteRecordingFinalization` is the only publisher of a terminal
overlay notification and it is only reachable from a live recording's stop. The controller's
`Finalizing` carries a 60 s expiry, so nothing superseded it; it was still being drawn 5 s after the
media process had exited cleanly.

**Why the probe is on the start path at all.** `[AVSyncProbe] cache=memory_miss ... entries=0` on
every single spawn. The disk cache was deliberately removed on 2026-06-18 in favour of a
process-memory cache documented as "one probe per fresh CE process" - but the media process is
disposable and exits after every recording, so the cache could never hit. Four probes across 70
minutes measured 28.6 / 29.0 / 28.6 / 30.4 ms on the same endpoint key: stable enough that
re-measuring per recording bought nothing and cost 3.2 s of the 3.6 s startup.

**Fixed (three changes, all in one commit):**

- `common/av_sync_latency_channel.{h,cpp}`: a controller-lifetime anonymous file mapping holding a
  small (key -> latency) table, inherited by each media child via `--avsync-latency-handle=`. The
  child reads it before probing and writes a fresh measurement back, so the probe now costs once per
  CE session. No disk file - the `audio_latency_cache.ini` ban is unchanged, and the endpoint key
  (which contains the device id) never reaches a command line. A seqlock makes a torn read a miss;
  every failure mode degrades to "probe again", never to a wrong latency.
- `CompleteAbortedRecordingStart` (media): `media_main_g_RecordingEverStarted` latches in
  `StartRecording`; both stop routes finalize an unlatched stop as `recording_canceled`, which
  publishes `RecordingCanceled` to both overlays and writes the manifest's terminal state. The
  shared-memory route clears the hook-facing state first or
  `CompleteRecordingFinalization`'s newer-recording guard would swallow it.
- Honest controller reporting: `[Controller] Recording started` on the inject ack is gone (that ack
  only proves inject set `cmdStartRecording`). The controller now logs the delivery, and
  `CheckChildProcessHealth` logs `Recording is live` with the elapsed startup time when it observes
  media's published `isRecording`. A stop while the start is still pending is logged with how long
  it had been pending. `main_g_RecordingStartRequestTick` is disarmed by every idle transition.

**Tests:** `tests/test_av_sync_latency_channel.cpp` (11 cases: reuse across processes, per-endpoint
keying, overflow, oversized/empty key refusal, incompatible block, torn read, unterminated entry)
plus five source-inspection cases in `tests/test_recording_start_feedback.cpp`.

**Unvalidated on hardware.** No run yet confirms the warm second recording, the canceled
notification, or the `Recording is live` timing. The obvious check is a session with two short
recordings: the second should log `cache=session_hit` and no `[AVSyncProbe] probing:` line, and a
sub-second recording should end on "Recording canceled", not "Finalizing recording...".

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
