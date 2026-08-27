# llm-wiki Log

### 2026-08-27 - Portal RTX override audit: upstream Remix MFG scheduling and Vulkan-native Reflex

Sessions `20260826_162652`, `20260826_225500`, and `20260826_232211` proved the split renderer received the resolved
profile. FIFO VSync,
forced AF, SR preset M, RR preset F, FG preset B, all four runtime folders, the process-local DLSS indicator, and the
NVIDIA LOD-spread patch reached their real consumers. The later run gives direct application proof: both immediate
swapchains were changed `present mode 0 -> 2 (FIFO)` and five live samplers logged their before/after 16x state. Thus
an unblocked FIFO present call or FG output above the base rate is not evidence that CE missed the swapchain override.

The runs exposed three distinct gaps. The first factor fix wrote a stale bare compatibility key; CE corrected this to
NVIDIA's official `DLSSG.MultiFrameCount`. Session `20260827_155554` then falsified the assumption that this is the
whole control boundary: the DLSS indicator showed configured 3x, yet changing Remix's menu 2x -> 3x still raised real
FPS. NGX consumes one `MultiFrameIndex` per generated evaluation; changing its count cannot make the host schedule the
missing evaluation. Remix schedules earlier through `rtx.dlfg.maxInterpolatedFrames`. Its bridge legitimately resolves
`remixapi_InitializeLibrary` and calls the returned `SetConfigVariable`; CE now wraps that interface setter and forces
the upstream option, with edge-triggered NGX mismatch reassertion for internal menu paths. No synthetic D3D9/Remix
initialization and no title/executable rule were added. Runtime confirmation of real output remains pending.

Remix's paired Present samples (~21.6 ms then ~0.2 ms) also explained the odd basic limiter: logical base rendering was
only ~46 fps, below the old unscaled 100 target, so no wait was correct even while 2x/3x output appeared near 92/138.
Every limiter mode now treats the configured general cap as final output: 100 with 3x paces about 33 base fps. Finally,
the present semaphore was signalled by a compute-only queue, whose own wait came from graphics. Session `232211`
proved the remaining limiter gap explicitly: graphics producer thread 8184 differed from present thread 27228, so CE
withheld the marker to avoid an illegal cross-thread queue borrow. Topology learning now retains the exact bounded
graphics-signal semaphore ring, and the matching `vkQueueSubmit*` wrapper appends the prerender marker from the owning
producer thread. Vulkan native Reflex remains preferred with timer fallback. No Portal/Remix executable-name rule was
added.

### 2026-08-25 - RTX Remix crash: synthetic D3D9 probe initialized a second renderer

Portal RTX did not have a missing-whitelist or generic injector failure. Literal session enumeration found the earlier
`20260825_190436` bridge-target run and its CE/external dumps; the later submitted `192442`/`194911` runs targeted the
32-bit `hl2.exe` client and exited during speculative x86 DX12 bootstrap. The actual renderer process is
`NvRemixBridge.exe`: it owns the x64 Remix `d3d9.dll`, CE Vulkan layer, Vulkan device, and WSI swapchain. The follow-up
`20260825_202150` run proved the remaining routing failure: the profiled `hl2.exe` connected normally, but the bridge's
layer logged `Process not whitelisted - layer dormant`, then forwarded all real swapchain calls without overlay/capture.

CDB plus the layer/hook timestamps establish two generic CE violations. HookThread entered
`GetD3D9PresentAddresses -> Direct3DCreate9Ex -> Remix d3d9` after `CheckAndInstallHooks` had already established Vulkan
ownership. That synthetic device negotiated a second Vulkan instance/device on the hook worker while the real Remix
renderer was still initializing; the external dump then faulted on a null read in `d3d9!remixapi_InitializeLibrary`.
Meanwhile NVIDIA's real Vulkan WSI swapchain traversed CE's pre-installed global, inline, and deep DXGI creation hooks,
which treated the ICD-private object as a game DX12 swapchain and began installing Present/colour-space hooks.

Fix: a resident CE Vulkan-layer module suppresses speculative D3D/DXGI setup before hook IPC classification; residual
`CreateSwapChain*` hooks exact-pass-through once layer ownership is visible; Vulkan-owned and non-system D3D9 runtimes
never receive synthetic D3D9 factory/device probes; false-positive D3D12 runtime presence no longer starts the x86 DX12
bootstrap over such a D3D9 translator. The Vulkan layer now owns the final overlay/capture/screenshot/limiter boundary
for translated D3D9. Policy and source regressions cover early suppression, the Remix topology, and residual-hook gates.
The Vulkan eligibility check now also recognizes a non-whitelisted direct child when its live parent PID equals the
host's active source PID or its exact pre-injection profile target, and that parent executable still matches the
published whitelist. Session `20260825_203627` falsified the first source-only implementation: the bridge checked at
`20:36:31.904`, 41 ms before remote `LoadLibrary` finished and published the parent as `sourcePid`. The host now writes
the selected target PID into versioned discovery memory in its pre-injection callback; that callback ran 272 ms before
the bridge check in the falsifying session. This generic lineage proof lets a client profile follow its final child
renderer while rejecting unrelated helpers and stale/unverified parents; there is no Portal/Remix executable-name
exception.

Build 0.1.6266 and session `20260826_020732` runtime-confirmed that Portal RTX no longer crashes and the Vulkan overlay
is visible. The remaining `GPU --` / absent VRAM rows were a distinct provenance mismatch: `sourcePid` correctly stayed
on profiled `hl2.exe` (PID 3944), while the live bridge (PID 22668) owned RTX 5070 LUID `0xC88E`. The Vulkan metrics
collector stamped the bridge PID, but sensors accepted an exact LUID only from the source PID and found no GPU-engine
evidence under the non-rendering parent, so it published validity `0x0`. Sensors now accept a graphics LUID from the
selected source or a live direct child whose process-table parent is that source, retain the parent as config/telemetry
publication identity, and log publisher PID, parent PID, and eligibility. This reuses the generic split-renderer lineage
boundary rather than adding a Remix name rule.

### 2026-08-24 - RoboCop NGX override crash: CE trusted a foreign GetProcAddress wrapper

The build 0.1.6258 user dump is a deterministic `0xC00000FD` recursion, not an NVIDIA implementation fault.
`_nvngx.dll` was the queried driver module, but the pre-existing `GetProcAddress` chain returned
`NVSDK_NGX_D3D12_GetFeatureRequirements` from game-local `version.dll` 4.5.2.2. CE then inline-hooked that foreign
wrapper as if it were the core export. The dump contains 6,011 returns through
`Hooked_GetFeatureRequirements_D3D12`, exactly `0x7d0` stack bytes apart, across nearly the complete 12 MiB thread
stack. `KERNELBASE!WideCharToMultiByte` was only where the exhausted stack finally faulted; the repeating chain is
`version.dll` / Streamline / CE. The NVIDIA core and plugin frames do not own the faulting recursion.

Generic coexistence fix: NGX inline installation now resolves the core image's PE export table directly instead of
calling an interceptable `GetProcAddress`; filtered dynamic hooks preserve results owned by a different module or an
unmapped thunk and emit a rate-limited ownership diagnostic; the requirements wrapper has a thread-local re-entry
fuse so any future malformed interceptor chain fails the one capability query instead of crashing the game. Focused
coverage exercises direct resolution, foreign-owner policy, and nested-gate ownership. The exact third-party proxy
binary was unavailable after the submitted session, so same-title runtime confirmation remains manual.

### 2026-08-24 - RR capability verdict is runtime-stack-dependent: full override flips Available 0->1

User report (submitted diagnostic logs, kept out of the wiki by name): `force_ray_reconstruction=on` did nothing in
RoboCop (build 0.1.6258). The CVar write held
(writeThrough=1, never drifted), but NGX answered `GetFeatureRequirements(Feature 13) = 0xBAD00012 FAIL_NotImplemented`
(support bitmask 16 = NotImplemented; minArch=10 is garbage - the struct is only valid on Success) and
`SuperSamplingDenoising.Available = 0`; Feature 13 was never created while SR (1) and FG (11) created/evaluated fine.
That profile had configured ONLY `dlss_rr_dll_path` (single-snippet DLSS Swapper-style folder); every sl.* module came
from the game's old bundled stack.

Decisive A/B on the SAME machine/driver/GPU/game: local validation session `robocopnooverlayscaling` (build 0.1.6223)
with all four paths
(`dlss_sr_dll_path`, `dlss_rr_dll_path`, `dlss_fg_dll_path`, `streamline_dll_path`) pinned to one complete NPI folder
read `SuperSamplingDenoising.Available = 1` through the same GetI hook and created+evaluated Feature 13. Conclusion:
NGX's RR capability verdict is established by whichever coherent runtime stack initializes NGX in the process, not only
by driver/GPU; a lone modern snippet inside an otherwise-old stack does not flip it (and the requirements-probe call was
not even observed in the healthy session). Corrections: my earlier "capability answers come from driver-store NGX core,
overrides cannot change them" reasoning was empirically wrong; the wiki no-spoof policy stands, but "unsupported" must
first mean "incoherent partial override" before it means impossible. Open question: the exact internal trigger inside
NGX init (SDK-version negotiation vs snippet validation vs per-app deny list). Actionable: stage ONE complete modern
set and point all four override paths at it.

### 2026-08-23 - Manual pre-release from local packages (v0.1.6258)

Published a GitHub **pre-release** from the already-built local `build/packages` archives instead of dispatching
`release-stable` (first use of this path; stable releases keep using the action). Procedure and gotchas:

- Identify provenance via `build/verification/<ts>_build_<n>/verification_manifest.json` (`package_archives: passed`),
  not the top-level `latest_*` pointers, which may reference a later run.
- Run-dir `verification_summary.txt` / `verification_manifest.json` written by builds predating the manifest-redaction
  code contain raw `C:\Users\<user>` paths — never upload them as-is. Stage copies, scrub both spellings (plain and
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
