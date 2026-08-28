# llm-wiki Log

### 2026-08-28 - Vulkan DLSS-G FIFO requires the final DXGI Present contract

Portal RTX sessions `20260828_014434` and `20260828_022342` both showed CE changing the driver-bound swapchain from
Immediate to FIFO, yet 3x DLSS-G still arrived in three-output bursts and the latter run visibly tore. The temporary
refresh-derived 144 FPS limiter merely paced one 48 Hz group at `vkAcquireNextImageKHR`; it was removed and never
committed. Aggregate 144 FPS was not VSync and made frame pacing worse.

NVIDIA's public `sl.interposer` source established the first missed boundary: its exported `vkCreateSwapchainKHR`
invokes DLSS-G's before hooks with the original create info, then calls the downstream Vulkan dispatch. CE now hooks
that stable export and substitutes guaranteed-supported FIFO before Streamline sees the call; the existing layer still
enforces the downstream call.

Session `20260828_162056` then falsified the assumption that this was sufficient. Its bounded diagnostics proved FIFO
on both sides (`before Streamline DLSS-G hooks` and downstream driver `presentMode=2`), yet generated output remained
about 158.8 FPS with repeated ~18.3 ms / ~0.2 ms / ~0.2 ms bursts. The bridge also resolved the system
`CreateDXGIFactory*` exports late. NVIDIA's Vulkan WSI is implemented over an internal DXGI flip swapchain, so the
remaining real VSync contract is that final swapchain's `Present`/`Present1` call.

For a resident CE Vulkan layer with explicit `vsync=fifo`, the hook now registers only those system-DXGI factory
exports in the existing `GetProcAddress` router. It returns the real factory unwrapped, patches only its real
`CreateSwapChain*` vtable methods, then patches only Present slots 8/22 on the returned real swapchain class. The
final detours call the existing Streamline/NVIDIA predecessor with `SyncInterval=1` and
`DXGI_PRESENT_ALLOW_TEARING` cleared. They do not modify descriptors, create probe objects, wait, set maximum frame
latency, or run CaptureEngine's ordinary DXGI overlay/capture policy; this preserves the DOOM Eternal ICD-thread
deadlock invariant. Logs identify the predecessor module and rate-limit the incoming/final Present arguments.
NVIDIA still does not officially support DLSS-G VSync on Vulkan, so Portal field validation remains required; there
is deliberately no timer/Reflex/refresh-rate cap fallback.

### 2026-08-27 - FPS limiter: FG-aware output-group admission fixes Portal RTX cap escape (130 configured, ~146 observed)

Session `20260827_211303` (build 0.1.6280, Portal with RTX Remix, `FpsLimiter.general_enabled=true`, cap 130, mode
`basic`, DLSS FG 3x) falsified "the limiter is disabled": it resolved 130, activated in the bridge, and its clock was
exact (~43.0 FPS local cadence) - but the callback stream reached 146.1/s (167 peak), with 92 six-callback / 31 five /
33 four batches beside 531 clean 3-batches and `activeDedup=1458` at 600 paced frames (ideal 2 bypasses/frame = 1200;
the ~258 excess = ~86 admitted extra groups explains the ~17-18 FPS overshoot). Root cause: while FG was active,
`strictGrid = gateEveryPresent && !IsFGActive()` disabled serialized per-presentation admission and the time-based
0.5-2 ms `activeDedup` window decided what "belongs to the current group" - the next real 3-frame group arriving
inside the window was indistinguishable from generated spillover and was admitted unpaced. The submit-thread mismatch
detection route had also switched pacing to `vkAcquireNextImageKHR` silently (no log), so acquire-time pacing was
invisible in the session.

Fix (all in the limiter + Vulkan layer, no game/executable/image-count/timing branches):

- `ce::fps_limiter_policy::OutputGroupAdmission`: deterministic multiplier-ordinal classification (`pace_group` vs
  `pass_generated_slot`) for real final-boundary callbacks; never reads a clock. Generated slots pass through a fast
  path that never touches the cadence mutex; owners block on it. Replaces the FG-active dedup escape; FG-off strict
  grid (Strange Brigade multi-present) and DXVK Present/PresentEx legacy dedup are unchanged.
- Admission epoch key compared BEFORE classification: first callback after any activation/deactivation, target/source
  change, FG on/off, multiplier change, IPC/session reset, or pacing-boundary move owns a clean slot.
- Exact rational group cadence: interval = `QPC_frequency * cadenceScale / configured_target` with Bresenham
  remainder (130/3 = 43.333... groups/s, zero drift); cadenceScale = FG multiplier for final-output observers, 1 for
  inject capture sync. Floored integer target remains only for integer driver APIs and legacy sites; logs now show
  `group=130/3`.
- `hook/vulkan_layer/vulkan_present_boundary.h`: both async-detection routes log edge transitions (submit-thread was
  silent), boundary-identity logs report QueuePresentKHR vs AcquireNextImageKHR with swapchain/FG/grouped, detection
  edges reset output-group admission, 120-frame stats extended with boundary/paced/generated/resets/skips deltas and
  a concurrentSkip invariant-violation tag.
- Tests: new `tests/test_fps_limiter_output_groups.cpp` (pure ordinal sequences incl. the 3x six-callback burst,
  2x/3x/4x windows, reset semantics, exact 130/3 + 100/3 + 141/4 sums, overflow guard, integration bursts, transition
  resets, native post-present arming by owners only, capture-source scale selection, hitch recovery, 4-thread
  concurrent admission with zero contention escapes); `GateEveryPresentDefersToDedupWhileFGActive` (the bug as a
  requirement) replaced by `GateEveryPresentUsesGroupedAdmissionWhileFGActive` in the moved-out unit;
  `PerformanceMetrics` trace test proves ~130 convergence and unclamped telemetry.

Portal RTX matrix validation (2x/3x, non-divisible caps, toggles, VSync variants, Reflex smoke, FG-switch overlay
capture continuity) remains required at runtime. Derived numbers: temp/fpslimitfix-notes.md (not committed).

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
`remixapi_InitializeLibrary` and calls the returned `SetConfigVariable`. Session `20260827_162905` then falsified the
first interception lifecycle: CE armed its filtered lookup route at 16:29:11.593 and verified the provider at .638,
but never captured the interface; repeated NGX mismatches showed the setter remained unavailable. Vulkan negotiation
had already begun at .448, and Remix initializes its public API before that boundary. NVIDIA's public bridge source
and disassembly of the shipped binary confirm the returned 0xA8-byte interface is copied into writable global image
storage. Session `20260827_202629` falsified CE's attempted table recovery too: build 0.1.6278 repeatedly found no
candidate, never captured a setter, and therefore only forced downstream `DLSSG.MultiFrameCount`; the raw menu change
to one generated frame remained observable at 20:27:20.309. CE now negotiates its own private function table through
the official initializer using exact known 0.6.4/0.5.1 API versions, validates slots 10-12 as readable code owned by
the pinned provider, and immediately calls the returned setter. NVIDIA's source and shipped machine code both show
that this API only fills the append-only table; it does not create a device or renderer. An isolated call against the
shipped provider returned success for both negotiation and `rtx.dlfg.maxInterpolatedFrames=2`, while an unknown key
correctly failed. Future-initializer interception and NGX mismatch reassertion remain; the latter also runs at the
final `EvaluateFeature` parameter boundary because old x64 Remix helpers can bypass earlier hooked parameter setters.
No binary offset or title/executable rule was added. Runtime confirmation of real output remains pending.

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
