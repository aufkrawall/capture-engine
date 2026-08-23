# llm-wiki Log

### 2026-08-23 - Manual pre-release from local packages (v0.1.6258)

Published a GitHub **pre-release** from the already-built local `build/packages` archives instead of dispatching
`release-stable` (first use of this path; stable releases keep using the action). Procedure and gotchas:

- Identify provenance via `build/verification/<ts>_build_<n>/verification_manifest.json` (`package_archives: passed`),
  not the top-level `latest_*` pointers, which may reference a later run.
- Run-dir `verification_summary.txt` / `verification_manifest.json` written by builds predating the manifest-redaction
  code contain raw `C:\Users\<account>` paths — never upload them as-is. Stage copies, scrub both spellings (plain and
  JSON-escaped `\\`), rename to `latest_summary.txt` / `latest_manifest.json`, assert zero residual hits.
- Pre-push secret/name audit of unpushed commits caught fixture data in `tests/test_log_privacy.cpp` plus wiki/comment
  mentions of a private recording folder; rewrote the unpushed commits via fixup + autosquash before pushing (pushed
  history must stay free of account names per project constraints).
- `gh release create <tag> --prerelease --target <sha>` requires the FULL commit SHA; short SHAs fail with HTTP 422
  `Release.target_commitish is invalid` even when the commit exists remotely. Archives came from `build/packages`
  byte-identical; tag version must equal the binaries' embedded build identity (0.1.6258). Tag target: the rewritten
  HEAD whose tree matches what the build compiled (redaction code was in the working tree at build time).

### 2026-08-23 - Log privacy: account names and output paths no longer logged

Users sharing diagnostic logs raised privacy concerns; auditing `installed/captureengine/logs/example` found the
Windows account name in 43 lines across 6 files (VulkanReg manifest/baseDir paths, screenshot save path, inject
`logsPath`/DLL-validation paths, logger session discovery, NVNGX/Streamline DLL paths, perf-CSV init,
`session_manifest.txt` `session_dir=`) and the user-chosen recording folder (`H:\...\capture_*.mkv`) in the media
log.

Fix (all funnels covered; no call site can forget):

- New header-only `common/log_privacy.h` (`ce::privacy`): `RedactUserAccountComponents` masks the account token of
  `\users\<account>` prefixes with `*` (case-insensitive marker match, marker spelling preserved). Deliberately
  **length-preserving**: an earlier compaction prototype corrupted adjacent bytes for accounts shorter than a
  placeholder and could grow formatted lines past funnel buffer capacity — tests caught both. CollapsePathForLog
  collapses user-configured output paths to root + leaf (`H:\...\capture.mkv`, UNC server/share collapsed too).
- Wired centrally: `common/logging.cpp` `Log()` now formats into a stack buffer (heap fallback via `va_copy` for
  oversized messages) and redacts before fwrite; `hook/common/hook_common.cpp` `LogToFileAtomic` redacts before the
  SHM ring / direct-file fan-out (covers hook_debug.log, nvngx_debug.log, and the logger-service consumer); Vulkan
  layer `EarlyLog`/`LayerLog`/`LayerReportIncompatibleDiscovery` redact their buffers; `session_manifest.txt`
  redacts `session_dir=`.
- Targeted leaf-collapse at every log site printing capture/screenshot output paths (video encoder staging/open/
  publish/mux-close/probe/cleanup, audio-only muxer start/stop, screenshot Saved lines, reserved-capture-output
  fallback warning `configured=`).
- Kept deliberately: CPU/GPU model + VRAM + DPI (high diagnostic value), game process/profile names (per-game
  diagnosis; it is CE's own config), PIDs/LUIDs/handles, timestamps. Crash dumps still contain paths by nature —
  unchanged scope.
- Coverage: new `tests/test_log_privacy.cpp` (13 units: masking, case-insensitivity, length preservation,
  bounded-input safety, collapse roots incl. `\\?\`/UNC/relative/URL-ish). clang-tidy baseline scope refreshed to
  643 TUs (0 warnings). Gates: incremental product build, full unit suite + Python self-tests, lint — all green.

### 2026-08-22 - Injection DLL integrity gates wired up (audit follow-ups)

A full security audit found that the injection subsystem's designed DLL-integrity protections were inactive:
`ValidateDllSecurity` (install-dir containment + broad-writability ACL check) and `VerifyDLLHash` were dead code,
and the early-APC path (`InjectEarly`, the primary route for launched games) skipped every check that `Inject()`
did have. Shipped releases are built without `--production`, so `CE_PRODUCTION_BUILD` is undefined and
signature enforcement was advisory-only anyway; README now states this explicitly (trust = GitHub attestations
+ reproducible builds until Authenticode signing ships).

Changes (commit `6f4ba8b4`, all additive, dev builds stay warn-only so local iteration is unaffected):

- `ValidateDllSecurity` is now called by BOTH `Inject()` and `InjectEarly()` before any remote load.
  Production builds fail closed on out-of-app-dir paths or broadly writable DLLs; dev logs and continues.
- `InjectEarly()` mirrors `Inject()`'s signature gate: `CE_PRODUCTION_BUILD` refuses invalid signatures;
  dev warns and honors `SKIP_DLL_VERIFICATION=1`.
- `VerifyDLLSignature` converts the ANSI path with `ce::injection::AnsiPathToWide` (`CP_ACP` +
  `MB_ERR_INVALID_CHARS`) instead of byte-wise char->wchar_t widening, which sign-extended bytes >= 0x80 and
  would verify a mangled name on non-ASCII install paths. Empty conversion fails closed.
- Launcher (`main_controller.cpp`) and inject child (`inject_main.cpp`) fail closed on truncated/unresolvable
  `GetModuleFileNameA` results when deriving hook-DLL/config paths: launcher resumes without injection and
  logs `[Launcher] Cannot resolve the application directory reliably`; inject child exits 1 with a clear log.
- Dead code removed: `VerifyDLLHash`, the `.hash` sidecar logic, member `ComputeFileHash`, and an unused static
  hash helper. Pure helpers now live in `captureengine/injection_path_policy.h`
  (`IsPathInsideDirectory`, `AnsiPathToWide`) with unit coverage in `tests/test_injection_path_policy.cpp`
  (sibling-prefix rejection like `C:\appdir2` vs `C:\appdir` is locked).

Audit items deliberately deferred near release: pip hash-pinning of bootstrap lint tools (build-env change),
trace-log default / crash-dump retention policy (user-visible product change), real Authenticode signing
(needs cert infra), same-user named-object hardening (accepted trust boundary). x86 CFG/ASan gaps remain
documented toolchain limitations. clang-tidy baseline scope auto-refreshed to 642 TUs (0 warnings).

### 2026-08-22 - RR quality presets no longer force the RR denoiser

`ray_reconstruction_optimal_settings` is now a nested `off|light|medium|full` quality preset. Light disables the
three requested Lumen/SSR temporal reconstruction paths, medium adds full-resolution Lumen reflections, and full
adds the former remaining Lumen/VSM/MegaLights values. `r.NGX.DLSS.DenoiserMode=1` was removed from this bundle;
only `force_ray_reconstruction=on` selects RR. Legacy `on` remains a `full` alias.

`custom_cvar_overrides` adds typed final precedence for every existing `kSpecs` CVar, including case-insensitive
canonical names and normalized aliases such as `tonemapper_sharpen`. The host rejects unsupported/mistyped entries
before publication and sends a spec mask plus raw typed values to the hook. This grows `SharedGraphicsConfig` from
420 to 688 bytes and moves the shared ABI and every versioned mapping/event name from 44 to 45.

### 2026-08-22 - Device notification bypassed the legacy teardown boundary

Witcher 3 session `20260822_185158` started and rendered its loading screen, then raised a real
`0xc0000005` null read in the title at `123.exe+0x1610976`. The bridge had progressed much farther,
but the module and log timelines proved it still had not performed the requested NGX upgrade:

- Streamline 2.12.128 finished initialization at `18:52:05.590`; legacy shutdown and NGX retirement
  followed at `18:52:05.592`. Both retirement releases succeeded but both images stayed resident.
- The dump contained one SR and one FG image, but they were the game's `nvngx_dlss.dll` 3.1.1 and
  DriverStore `nvngx_dlssg.dll` 310.2.1. SL2 had requested the configured absolute paths, correctly
  reused the already-resident images rather than mapping duplicates, and thereby acquired the extra
  references which made the later safe retirement release insufficient.
- The bypass was `NotifyD3D12Device`: unlike every bridged import, the queue-derived device path
  called `EnsureRuntimeReady` directly while legacy quiescence was still pending.

Legacy quiescence is now an explicit Pending/Running/Complete/Failed state machine. Every V2 bring-up
route, including device notification, must cross it; `EnsureRuntimeReady` independently refuses to
load V2 until it is Complete. The bridge stays inactive throughout import rewriting, then publishes
Pending before Active; an early thunk entrant remains on V1 and cannot tear it down under the rewrite.
Concurrent game callers wait without polling. A same-thread call
re-entered by `slShutdown` stays on V1 so teardown cannot deadlock itself. After retirement CE
preloads and pins the configured SR/FG images, verifies every physical copy belongs to that folder,
and only then publishes Complete. The inventory is emitted before any V2 interposer load. The next
run should therefore show only configured 310.7.128 SR/FG images at that boundary.

That session's first translated DLSS evaluate returned `eErrorMissingConstants` six seconds before
the game-side null read. Causality is not established; first set-constants/evaluate diagnostics now
record input frame, actual V2 token frame, viewport and result so a remaining translation error can
be distinguished from the proven mixed-generation problem.

### 2026-08-22 - A duplicate refusal must not replay the duplicate's absolute path

Witcher 3 session `20260822_182415` reached Streamline 2.12.128, bound the native device and
resolved all requested feature functions, but the external fatal-exit dump proved that the game's
`nvngx_dlss.dll` 3.1.1 and the configured 310.7.128 image were both live. DriverStore
`nvngx_dlssg.dll` 310.2.1 and configured 310.7.128 were live too. The retained device was healthy
at the 11.7-second capability probe, then reported `DXGI_ERROR_DEVICE_RESET` before the title's
final object request; the title again terminated with `0xe06d7363`.

- The duplicate guard logged "keeping the loaded copy" but returned an empty redirect. That only
  preserves the resident image when the caller originally requested it; SL2 requested the custom
  absolute path, so replaying the original request mapped the second image anyway. Duplicate
  refusal now returns the resident physical path through every loader front end.
- An eligible 1.x -> 2.x bridge preloads SR/FG from `streamline_dll_path` before the 15-slot import
  takeover and immediately patches the already-resident legacy SL modules' LoadLibrary imports.
  This lets an in-flight late 1.x `slInit` converge on the same NGX images as 2.x.
- The quiesce captures all live SR/FG feature images before calling legacy `slShutdown`. After a
  successful shutdown it releases each captured foreign image once, before SL2 initialization,
  while never draining an opaque loader count that another integration may own. Logs and bridge
  inventories include every physical SR/FG image, not only `GetModuleHandle`'s first answer.
- The duplicate NGX state is the concrete unsafe difference from the preceding run; attributing
  the device reset specifically to it remains an inference until a fresh title run proves the
  single-generation state and reaches the render loop.

### 2026-08-22 - Reuse the proven D3D12 device before a redundant creation can reset it

Witcher 3 session `20260822_174509` reached a healthy CE-owned Streamline 2.12.128 runtime, loaded
the custom-path DLSS, DLSS-G, Reflex and PCL plugins, bound a native D3D12 device, and applied the
configured SR/FG overrides. Before any translated feature call, its later `ID3D12Device1` request
returned `DXGI_ERROR_DEVICE_RESET` for both the LUID-matched and default-adapter attempts; the title
then threw `0xe06d7363`. The retained-device fallback ran too late because the failed driver call
had already reset the object it needed to recover.

- A compatible same-LUID repeated object request now checks device health and queries the requested
  interface before entering D3D12 at all. Different adapters, higher feature requirements,
  unsupported interfaces and unhealthy devices still take the native path.
- Cache identity comes from the created device's actual adapter LUID, including explicit tracking
  for default-adapter creations; the cache always follows the newest successfully created device.
- Streamline handoff deduplication now uses canonical COM identity rather than interface-pointer
  equality, and a failed `slSetD3DDevice` is not recorded as an accepted runtime device.
- A genuinely new device closes the feature-call gate until `slGetFeatureFunction` answers again;
  Reflex set/sleep entry points both remain in the retry-completeness condition.
- The dump and module inventory confirm that the statically imported 1.x interposer is mapped but
  inert; every live Streamline plugin and NGX binary came from the configured 2.x folder.

### 2026-08-22 - Vulkan compute presents need a compute-side overlay composite

DOOM Eternal's async-present mode changed the present topology from graphics family 0 to a
compute-only family 2 and reduced the swapchain from three images to two. CE's CPU and overlay
cost stayed effectively flat (about 8-9 us overlay GPU time), but the direct graphics render pass
inserted a compute -> graphics -> compute dependency round trip that the game does not have without
the overlay.

- On eligible non-graphics compute presents, render the overlay independently into a transparent
  sampled image on CE's reserved graphics queue, then blend only its occupied rectangle into the
  storage-capable swapchain image on the original compute/present queue.
- Gate the formatless compute shader on the application's existing swapchain storage usage and
  SPIR-V read/write-without-format capability (legacy device features, Vulkan 1.3, or the
  format-feature-flags2 extension), plus the exact format's corresponding feature bits when it is
  not core-guaranteed. Do not widen device features or swapchain usage; retain the direct
  render-pass fallback when any capability is absent.
- Keep the independent graphics submit free of game waits and prefer CE's queue, avoiding both the
  cross-engine critical path and serialization with the game's next graphics submission.
- Follow-up A/B/A session `20260822_165450` showed 138.48 / 140.08 / 138.56 FPS for async-on / off / on.
  There were no fallback or fence stalls; the remaining measurable difference was 33-34 us of overlay CPU.
  Cache the executable final-composite command per swapchain image while its occupied rectangle is unchanged,
  and retain compute wait scratch vectors across presents. A once-per-2048-frame CPU summary samples the steady
  record/submit phases every 128 frames and times only actual command-cache misses, so future runtime evidence can
  distinguish submission overhead without materially perturbing the present path.

### 2026-08-22 - Redundant D3D12 recreation can reuse the proven device

Witcher 3 `20260822_021816` successfully created a native D3D12 device, handed it to SL2, and
answered a later null-output probe from that proof. Eleven seconds later an object-producing
request with the same adapter/feature level failed with `DXGI_ERROR_DEVICE_RESET`; the default
retry failed identically. The title threw `0xe06d7363`.

- Keep a COM reference to the last successful native device per adapter LUID.
- On a device-lost-class failure only, satisfy the repeated request by querying that retained
  device for the requested interface after checking its removal reason.
- Normal first requests still create distinct devices; this is a recovery path for drivers that
  reject redundant validation they had just completed.

### 2026-08-22 - Bridged Reflex needs per-frame sleep, not just activation

Witcher 3 `20260822_015042` accepted the synthesized `eLowLatencyWithBoost` options, but SL2
kept every generated frame in
`eDLSSGStatusFailReflexNotDetectedAtRuntime - sl.reflex must be enabled and active`. A native 2.x
title also calls `slReflexSleep` once per frame; a 1.x title has no such export.

- The bridge now resolves `slReflexSleep`, enables it only while translated DLSS-G is on, and
  deduplicates it by game-frame token before common constants and again as an evaluate fallback.
- The statically imported 1.x interposer necessarily remains mapped after takeover. The post-quiesce
  log now states that its loader reference is inert and all live imports reach CE.

### 2026-08-22 - Null-output D3D12 probes need bridge-side support caching

Witcher 3 `20260822_014209` crashed during startup after a successful bridged device creation.
Its later `D3D12CreateDevice` request used the optional null output form, but both the
LUID-matched adapter and null/default retries returned `DXGI_ERROR_DEVICE_RESET`. The game treated
that failed redundant probe as fatal.

- CE now remembers each successful native D3D12 creation by adapter LUID and feature level.
- A later equal-or-lower ID3D12Device capability probe with a null output is answered `S_OK` from
  that proof instead of asking the driver again.

### 2026-08-22 - Bridged DLSS-G requires a synthesized Reflex activation

Witcher 3 `20260822_011315` reached DLSS-G present handling, but SL2 rejected every frame with
`eDLSSGStatusFailReflexNotDetectedAtRuntime - sl.reflex must be enabled and active`. The measured
1.x payload had Reflex mode zero even while the title used NVAPI Reflex separately; forwarding
that zero faithfully told 2.x that Reflex was off.

- While translated DLSS-G is on, the bridge forwards `eLowLatencyWithBoost`; when DLSS-G turns
  off, it restores off. An explicit Reflex call while FG is on is also promoted instead of
  overwriting the required signal.
- Repeat suppression remains process-wide because 2.x `ReflexOptions` has no viewport argument.
- This explains the earlier "options accepted but no FPS gain" state. The next validation must
  show `Achieved 'good' FC feedback state` without the Reflex-not-detected errors.

### 2026-08-22 - Streamline bridge tag lifetime and FG option deduplication

Witcher 3 `20260822_005204` reached the render loop and DLSS-G created its swapchain, but the
first SR evaluate returned `eErrorMissingInputParameter`. The bridge's arbitrary 16-entry tag
queue had already overflowed; evaluate then flushed a mixed/incomplete set. SL2 also logged a
Present race for every repeated `slDLSSGSetOptions`, while 1.x drives feature constants every
frame.

- `slSetTag` now translates immediately to deprecated 2.x `slSetTag` with a null command buffer;
  every translated tag is `eValidUntilPresent`, which the 2.x API explicitly permits without a
  command buffer. No cross-frame queue exists.
- Cache DLSS-G mode/generated-frame count per viewport and forward only state changes. Unchanged
  calls return success without provoking SL2's documented SetOptions/Present race.

### 2026-08-22 - Streamline bridge default retry must cover null-output capability probes

Witcher 3 `20260822_003944` still ended at the same adapter-bound reset, but the expected
`null/default retry` diagnostic was absent. The fallback was incorrectly gated on `ppDevice`
being non-null; `D3D12CreateDevice` allows a null output as a support probe, and this title uses
that form on its later request.

- Run the device-lost-class null/default retry for both object-creating calls and null-output
  capability probes.
- Log the actual output-pointer value in retry/failure diagnostics; never dereference an absent
  output just to clear it.

### 2026-08-22 - Fresh Streamline bridge adapters can still be reset; add scoped default retry

Witcher 3 `20260822_003051` created the first bridged D3D12 device successfully and resolved all
feature contexts, but its later `ID3D12Device` request still returned `DXGI_ERROR_DEVICE_RESET`
with feature level 11.0—even after resolving a fresh DXGI adapter by the requested LUID.

- Keep LUID normalization, but on device-lost-class failures only (`REMOVED`, `HUNG`, `RESET`,
  driver internal error), retry once with a null/default adapter.
- Log both attempts, both adapter pointers and the feature level. This keeps multi-GPU selection
  changes visible instead of silently substituting hardware.

### 2026-08-22 - Streamline bridge adapters need a fresh LUID-matched instance

Witcher 3 `20260822_001759` proved that native `D3D12CreateDevice` alone was not enough. The first
bridged device succeeded and feature contexts came up, then the later render-device request failed
again with `DXGI_ERROR_DEVICE_RESET` while reusing the game's adapter object after short-lived
factory/proxy generations had been destroyed.

- Before calling D3D12, read the requested adapter's LUID, create a fresh OS `IDXGIFactory4`, and
  resolve an equivalent adapter by that LUID. This preserves multi-GPU intent while avoiding
  dependence on another module's adapter/factory object lifetime.
- Device failures now include requested vs resolved adapter, feature level and IID.

### 2026-08-22 - Streamline bridge: native device handoff and NGX project identity

Witcher 3 `20260821_234606` isolated two bridge failures. V2's interposer returned
`DXGI_ERROR_DEVICE_RESET` for the game's later real-device request, which made the title throw;
and V2 turned a zero application ID into its production temporary ID, disabling all NGX-backed
features.

- Bridge `D3D12CreateDevice` no longer forwards to the V2 interposer. It creates through
  Microsoft's `d3d12.dll`, unwraps a factory-adapter proxy with `slGetNativeInterface` when
  necessary (balancing that call's AddRef), and explicitly hands each distinct native device to
  V2. Explicit devices supersede one another; queue-derived discovery remains fallback-only.
- Since late activation cannot observe the game's original 1.x `slInit` application ID, CE gives
  V2 a deterministic project identity from the host path plus host version. This avoids both a
  game table and embedding the local path in the identity sent to NVIDIA.
- Regression coverage pins the stable project-ID format and source-level absence of V2-targeted
  device creation. Focused `StreamlineBridgePolicyTest.*` passes; runtime confirmation in
  Witcher 3 is still manual.

### 2026-08-21 - Inject-overlay DPI: nearest display truth, not game-window awareness

RoboCop `20260821_224340` initialized the DX12 overlay at `dpiScale=1.00` on a 150% display. Its first
Streamline-created swapchain was 2560x1440; after Alt+Tab/fullscreen recovery it became 3840x2160, and warm
DX12 resize reuse preserved the font atlas that had been rasterized for the wrong scale.

- The adapter previously called `GetDpiForWindow()`. That reports the target window's awareness-virtualized
  DPI (96 for an unaware app), not the physical display scale. This is the same trap already fixed in the
  pseudo-overlay.
- Inject adapters now query `GetDpiForMonitor(MDT_EFFECTIVE_DPI)` for `MonitorFromWindow(referenceHwnd)` and
  use the shared pseudo-overlay fallback policy. The legacy `LOGPIXELSX` path remains only for systems without
  Shcore. Initialization logs the resolved monitor/DPI/scale.
- Do not infer DPI from backbuffer dimensions: logical-to-physical resolution changes are a presentation-mode
  change, while monitor effective DPI is the stable scaling input.
- Source regression: `OverlayDpiSourceTest.InjectOverlayScaleUsesNearestMonitorEffectiveDpi`. Focused tests
  pass; fresh RoboCop runtime confirmation is still manual.

### 2026-08-21 - Three new UE5 override families, and reading the types out of the binary

Added `[UE5]` `depth_of_field`, `dlss_super_resolution` (+ `dlss_super_resolution_quality`) and the HDR set
(`hdr_output`, `hdr_peak_luminance`, `hdr_paper_white`, `hdr_ui_luminance`, `hdr_min_luminance`,
`hdr_color_gamut`). No new machinery: each is a spec in `ce::ue5_cvar::kSpecs` with its own activation, installed
by the existing validated redirect. Full contract in [ue5-cvar-overrides.md](../ue5-cvar-overrides.md), which is
also where the whole UE5 section of `graphics-overrides-and-frame-pacing.md` moved (that page had reached the
800-line ceiling).

What is worth remembering from the work itself:

- **The CVar types were measured, not assumed.** Enumerating UTF-16 literals in a shipped UE5 binary
  (`G1R-Win64-Shipping.exe`) and disassembling the RIP-relative `lea` that loads each name lands on the
  registration call: UE registers int32 CVars through the console-manager vtable slot `+0x18` with the default in
  `r8d`, floats through `+0x10` with the default in `xmm2`. That is how `r.HDR.Display.MaxLuminance` turned out to
  be an **int** while `r.HDR.Display.MidLuminance` and `r.HDR.Display.MinLuminanceLog10` are **floats** - the
  symmetric guess would have written denormal garbage into an engine global, the same failure mode
  `r.MipMapLODBias` had. The help text sits next to each literal and supplies the semantics for free
  ("nit level for 18% gray", "0: Off", "Enable/Disable DLSS SR or RR at runtime").
- **Forcing DLSS on is not one flag.** `r.NGX.DLSS.Enable=1` reaches nothing in a TSR game: UE only routes a
  third-party temporal upscaler when the AA method is TAA and `r.TemporalAA.Upscaler=1`. The measured
  `r.AntiAliasingMethod` default in that build is 4 (TSR), so the on direction also writes
  `r.NGX.Enable=1`, `r.TemporalAA.Upscaler=1`, `r.AntiAliasingMethod=2`. The off direction stays minimal
  (`r.NGX.DLSS.Enable=0`) because killing NGX would take DLSS-G with it.
- **Depth of field uses the quality CVar, not `ShowFlag.DepthOfField`.** CE classifies bit-ref objects but does not
  write force bits, so a show-flag spec would resolve and sit inert.
- **Two settings that could contradict each other now have an order:** `display_gamma=srgb` writes an SDR
  `r.HDR.Display.OutputDevice`, and that write stands down while `hdr_output=on`.
- Version-conditional: UE 5.0 (Black Myth Wukong) has no `r.HDR.UI.Luminance`; it is reported missing and skipped.
  `r.HDR.UI.Level` was not added as a fallback because it is a multiplier, not nits, and the equivalence is
  unmeasured.
- ABI: nine appended `SharedGraphicsConfig` fields, `sizeof` 384 -> 420, `SHARED_MEMORY_VERSION` 43 -> 44 (mapping
  names renamed with it). `common/config_load_ue5.cpp` was split out of `config_load_core.cpp` so the `[UE5]`
  vocabulary is one unit.
- Stale-risk: **nothing has run in a game yet.** Proven from binaries, unit tests, and the engine's own registered
  help text only. First run should check the install/verify summary for the new names, and whether forcing
  `r.HDR.EnableHDROutput` mid-session is picked up at all (the engine reads it at swapchain creation, and a
  redirect never fires its console-variable sink).

### 2026-08-21 - sl.log answers it: the game's first D3D12 device is a throwaway

`20260821_163534` (0.1.6215) reached the render loop and the swapchain, then crashed in the same
place as the first bridged run - `Bridged_slSetConstants -> sl_interposer!slSetConstants+0x49 ->
0x0`. Streamline's own log, added the run before, names the cause outright:

```
d3d12Device.cpp:396[Release]   Destroyed D3D12Device proxy ... ref count 0
pluginManager.cpp:1331[initializePlugins] D3D or VK API hook is activated without device being
                               created, did you forget to call `slSetD3DDevice`
sl.cpp:1115[slGetFeatureFunction] 'kFeatureDLSS_G' has not been initialized yet.
```

**The Witcher 3's first D3D12 device is a capability probe it throws away.** Created through the
bridge at +1.9 s, proxied by Streamline, released at +2.3 s at ref count 0; the device it actually
renders with arrives seven seconds later. CE had marked the runtime ready on that first device, so
when the real one came through `SetV2RuntimeDevice` early-returned "already done",
`slSetD3DDevice` was never called with it, and the gate that exists to prevent this exact crash
waved the call through because CE had told it a lie.

Two rules, both generalising past this title:

- **Readiness is Streamline answering `slGetFeatureFunction`, never anything CE infers.** Not "we
  called slSetD3DDevice", not "the interposer created a device" - both were tried, both produced
  the same null call. `slGetFeatureFunction` returns a pointer out of the very plugin context
  whose absence makes `slSetConstants` jump through null, so it is not a proxy for the condition,
  it is the condition.
- **Hand over every distinct device, not the first.** A device is an action CE takes, never a
  conclusion CE draws. The interposer's device (Streamline's proxy, at the documented moment)
  wins over CE's queue-derived native one, and a later interposed device supersedes an earlier -
  which is what a throwaway probe requires.

The probe is event-driven: an epoch bumped when a device is handed over and when the game's frame
index moves, at most one `slGetFeatureFunction` per epoch, none once the answer is yes. A frame
boundary is a real state transition - it is what reaching the render loop looks like, and when
Streamline finishes bringing DLSS-G's context up around the swapchain - so it converges without a
timer.

**Reflex now translates too, and a phantom field went with it.** DLSS-G does not engage with
Reflex off, so refusing `slSetFeatureConstants(Reflex)` - which the bridge did from the start -
would have left frame generation configured and inert. Re-reading the measured payload from
`20260821_042540` shows the 1.x `ReflexConstants` is 8 bytes: `mode`@0 = 1, +4 always 0, and
from +8 the captures disagree with bytes that read `00 46 00 00 f6 7f 00 00` - a `0x00007ff6....`
module address straddling +8 and +12, i.e. a caller's saved pointer. The earlier "frameLimitUs@12
= 565" was that stack tail, which is precisely what the probe's own documentation warns about and
what was not heeded the first time. **A field that is only ever non-zero in captures where it
disagrees with itself is not a field.** Only `mode` is carried; the rest keeps 2.x defaults.

The mirrored 1.x structures moved to `streamline_bridge_v1_abi.h` - a different kind of claim
from the code that calls a documented 2.x API, and the translation unit had reached the size
ceiling anyway.

Corrections to the previous entry, from the same log: `featuresToLoad` **is** honoured (`Ignoring
plugin 'sl.deepdvc' since it is was not requested by the host`) - what was observed earlier was
Streamline probing each plugin's config and unloading what it does not need. And the
`20260821_161620` startup C++ exception did not recur; unexplained rather than fixed.

### 2026-08-21 - Second bridged run: the device reaches 2.x, and CE was binding it twice

`20260821_161620` (0.1.6212) confirmed both fixes from the previous entry and gave the cleanest
state so far: 15/15 slots taken over, the 1.x runtime shut down, `leaving sl.interposer.dll
unhooked` for the superseded module, CE's hooks landing on the 2.x interposer instead, Streamline
2.12.0 up, and `the game's D3D12CreateDevice reached the CE-owned 2.x runtime`. So the game's
device creation does go through `sl.interposer!D3D12CreateDevice` and does reach the bridged
runtime.

Two defects left, both mine:

**The readiness probe vetoed a direct answer.** `slSetD3DDevice(...) returned sl::Result=0 and the
runtime still reports no device` - the confirming `slGetFeatureFunction` probe had been made the
authority over the call's own return code. A probe failure proves nothing (it also fails while the
DLSS plugin is still coming up), so it may only ever confirm, never veto.

**CE bound the same device twice.** The 2.x interposer created and bound the device inside the call
CE was returning from, and CE then called `slSetD3DDevice` on it again - documented as "NOT thread
safe and should be called IMMEDIATELY after main device is created", issued from inside that very
creation, and through CE's own inline hook on that export. Streamline offers interposed creation
**or** `slSetD3DDevice` for a host that made its own device; doing both is a second bind, not a
belt-and-braces. The bridge now marks the runtime ready without calling anything when the
interposer created the device, and keeps `slSetD3DDevice` for the Agility SDK route
(`ID3D12DeviceFactory::CreateDevice`) that Streamline never sees.

Also: the device gate is now a plain atomic read. The version that asked the runtime on every call
would have put a `slGetFeatureFunction` - CE inline-hooks that export on the bridged interposer, so
it re-enters CE's own Streamline layer - in front of every tag and constant call on the render
thread, for as long as the device was missing.

**The run still ended in an unhandled C++ exception `0xE06D7363`**, seven seconds after the last
Streamline interaction, before the game presented and before it made a single feature call.
Unattributed: `ntdll!RtlUserThreadStart` caught it, so the throw site was fully unwound before CE's
pre-termination hook ran and the dump holds no trace. This game has a documented history of exactly
this exception shape at startup that the user reproduced with CE not injected at all
(`20260820_142322`) - which is a reason not to assume it is the bridge's, and equally not to assume
it is not.

Two changes exist to settle it next time. **`sl.log`, verbose, into CE's session directory at trace
log level** - CE's log says what CE did, not what Streamline made of it, and NVIDIA's own account of
plugin loading, device binding and feature init is the missing half. And **the hand-mirrored
`sl::Preferences` is gone**, replaced by the SDK's own struct: it had already been wrong once
(`BaseStructure` puts `next` at 0 and `structType` at 8, the reverse of how the declaration reads),
re-verifying it recurs every time the staged SDK moves, and the header is on the hook DLL's include
path anyway. The same argument that put the real `sl::Constants` behind the translation.

Noted for later: Streamline loads every plugin in the staged folder regardless of `featuresToLoad`
(`sl.deepdvc`, `sl.directsr`, `sl.dlss_d`, `sl.nis`, `nvngx_dlssd` at 40 MB), about 3.7 s inside the
game's first bridged call. Successive plugins load at the same base address, which is consistent
with SL2 probing each plugin's config and unloading what it does not need.
