# llm-wiki Log

### 2026-10-09 - Dynamic MFG factor lost to a startup race (Witcher 3 + ReShade)

- Session `20261009_110607` (0.1.7057): the in-game factor won. DRS answers were 2 (preset, forced mode) instead of 6;
  `sl.common` of the game was never wrapped. Passing neighbours (105819, 105938) had identical config/build.
  Race: CE preloaded override `sl.common` (hook thread, 13.984) while the game mapped its own (game thread, 14.001),
  between the startup sweeps and the LdrLoadDll hook (14.062); the loader notification saw it but did not patch.
- Fix: loader notification patches the `GetProcAddress` import of any DRS consumer inline + hook-thread sweep flag.
  Details and the open orphan-`sl.common` observation: `frame-generation/dlss-driver-settings.md`. Hardware run pending.

### 2026-10-09 - DLSS OFF lost overlay ownership behind distinct device views

- Session `20261009_101805` (0.1.7056, Witcher 3 with ReShade): two queue-derived device views alternated
  while the physical presentation route stayed live. ECL adoption treated pointer inequality as migration
  and cleared exact PostSL/normal/capture identities. First OFF at 10:20:05.637 blanked 97 presents;
  final OFF initially rendered, then discovery resumed after its 600-frame grace and blanked at 10:20:28.026.
  Device removal stayed zero and the confirmed queue remained retained; this is an ownership failure.
- Discovery is now fallback-only before GetDevice. Completed swapchain captures explicitly bind the
  queue/device before publishing exact proofs; replaced references retire outside the queue lock. No
  game/module special case, delayed recovery, backbuffer copy or weakened ownership gate is added.
- The native policy regression and both new real-hook flows fail on the original implementation. `FlowQueueBinding` adds
  distinct WARP COM device views, repeated DLSS ON/OFF beyond teardown grace, both Present methods,
  native return, explicit binding replacement and reference-balance checks. Closing build 0.1.7057 passes
  the full native suite, Python tool self-tests, all 36 FG flows and binary/privacy checks; its fresh installer
  is 39060080 bytes. Real-game ReShade retest remains pending. Current invariant: `overlay-rendering.md` queue ownership.

### 2026-10-09 - Automatic dump missed a CE fault consumed by game handling

- The same Witcher 3 startup session retained one unresolved CE access violation in its first-chance slot, but
  had no CE crash dump or crash log and ultimately exited with code zero. The manual dump's render stack had
  already returned to game code. Record-only first-chance handling relied on an unhandled filter or crash exit;
  those never arrived. Background freeze suppression also cannot substitute for preserving the original fault.
- The classifier now captures undebugged hardware faults whose instruction address is inside the module hosting
  CE's crash handler. `crash_first_chance::Install` caches that immutable image range before VEH registration;
  classification performs only atomic reads and range arithmetic. Foreign faults, breakpoints, managed and C++
  exceptions retain their existing behavior. The fault is also recorded for retry if immediate capture fails.
- The external-helper routing regression fails before the fix (zero calls), then passes with exactly one call and
  the original thread/address/registers. Module-identity and policy tests cover foreign/null/noncanonical
  addresses, debugger ownership and ignored software exceptions. Full build 0.1.7056 passes native tests,
  Python self-tests and all 34 FG flows and produces a fresh 39057612-byte installer. Real-game retesting remains pending.

### 2026-10-09 - Witcher 3 DX12/ReShade startup renderer type confusion

- Session 20261009_093652 (0.1.7053): second Present switches between a ReShade device view and the native view,
  causing descriptor-free backend replacement. The manual dump has no exception stream, but CE's first-chance
  slot retains the render thread's access violation in `OverlayAdapter::DestroyResourcesLocked`, inlined
  `DX12Backend::HasInlineUploadsInFlight`: it reads a noncanonical glyph-data pointer as a completion buffer.
  x64 CDB verified the archived CE PDB GUID/age and loaded matching Microsoft ntdll symbols.
- `DX12DescFreeBackend` derives from `RendererBackend`, not `DX12Backend`. The shared DX12 label was used as
  proof for five unsafe casts, including retirement. Adapter binding now caches an explicitly advertised texture
  interface; descriptor-free resources and generic virtual upload-slot dispatch remain intact.
- A native custom-renderer regression fails before the fix because texture helper calls overwrite unrelated
  storage. Added poisoned-storage shutdown/rebind checks and real WARP texture/custom lifecycle coverage to the
  descriptor-free format probe. Closing build 0.1.7056 passes the full native suite, Python tool self-tests and all
  34 FG scenarios; fresh installer is 39057612 bytes. The real-game cold-start check after installation remains pending.

### 2026-10-08 - Streamline UI-tag log flood (Witcher 3 Remastered)

- Session 20261008_220437: 11466 `Official UI tag record opportunity` + ~11.5k tag lines (85% of hook_debug.log) although the lines were already
  gated. W3 sends single-tag `slSetTagForFrame` calls cycling 13 buffer types, all one stream, so each call was a "change". Tag types now key the
  stream, 64 slots, per-stream heartbeat (details in `regression-testing-and-logging.md`). 0.1.7052 cut it to 1915 lines of 4438 (20261008_221807):
  types 0/1 alternate set (1280x720) / clear (null resource) calls; 0.1.7053 adds "has a resource" to the stream. Expect ~15 tag records per session.
- Same session confirmed dynamic MFG end to end (all five DRS keys answered by `sl.common`, multiplier 2x/3x/4x at ~138 fps on 144 Hz, no dump).

### 2026-10-08 - Dynamic MFG, second cause: cached NvAPI pointers in sl.common

- Session 20261008_214202 (0.1.7050): the startup sweep patched `sl.common`'s `GetProcAddress` import, yet no lookup was routed and
  `NvAPI_DRS_GetSetting` was still wrapped only for `nvngx_dlssg`. Disassembly: `sl.common`'s static NvAPI layer caches the driver's
  `nvapi_QueryInterface` in `.data` from `slInit`, before CE attached. 0.1.7051 retargets such cached copies (`RetargetCachedNvApiPointers`,
  restored in `ShutdownIATHooks`); details in `frame-generation/dlss-driver-settings.md`. Hardware run pending; expect
  `had already cached the driver's NvAPI entry points; retargeted nvapi_QueryInterface x1` for `sl.common.dll`, then
  `wrapping NvAPI_DRS_GetSetting for ...sl.common.dll (streamlinePlugin=1` and `answered` lines for 0x10562D0F / 0x10CF4125.

### 2026-10-08 - Dynamic MFG never reached Witcher 3 Remastered's own Streamline core

- Session 20261008_211749 (0.1.7049, `dlss_fg_mode=dynamic`, native SL 2.14.1): only `nvngx_dlssg.dll` was wrapped, so the preset and forced
  mode were answered but the count/target/VSync keys (read through `sl.common`) never were. `sl.common` was mapped before CE's loader
  notification and the Streamline-skipping IAT sweep, so its `GetProcAddress` import was never patched. Added a pinned startup sweep over
  already-loaded DRS consumers plus per-module logging (`frame-generation/dlss-driver-settings.md` "Consumers mapped before CE arrived").
  Hardware run pending; look for `via=startup sweep` and `wrapping NvAPI_DRS_GetSetting ... sl.common.dll (streamlinePlugin=1`.

### 2026-10-08 - Overlay fonts oversaturated for one HDR10 frame (Witcher 3)

- Session 20261008_184332: the game flipped `R8G8B8A8` -> `R10G10B10A2` + HDR10 -> `R8G8B8A8` (3 presents, the middle one 288 ms).
  CE's ResizeBuffers entry is refused behind Steam's overlay, so CE never saw it; the descriptor-free pipelines stayed `fmt=28` while the RTV
  was `fmt=24`. Fixed with per-format pipelines + live format sync (`overlay-rendering.md` HDR invariants). Hard evidence: with the retarget
  disabled the flow probe gets D3D12 error 613 from the debug layer. In-game re-test pending.
- Also fixed (pre-existing, found as a ~1/12 flake of `FlowDLSS.NativeReturn...` under load, identical on HEAD): the post-process pass cached its RTV
  by back buffer pointer, stale after a swapchain replacement (`post-processing-sharpen.md`). Always rewritten now.
- HDR10/scRGB frames of a gamma-only config were `pass-failed` in the DX12 post-process ledger (`SharpenDX12PresentedFrame` mapped the
  deliberate `gamma_hdr_passthrough` idle to `PassResult::Failed`). Fixed with `RequestHasWork` (`post-processing-sharpen.md`);
  the SDR/HDR10/SDR flow scenario asserts the full ledger again.

### 2026-10-08 - Gamma build crashed Witcher 3 on DLSS FG off

- Session 20261008_172629 (0.1.7034): CreateRTVs got DEVICE_REMOVED 3 ms after the first FG-off frame; the game's
  own int3 message says GPU crash. Earlier gamma builds (7032, same FG-off sequence x3) survived; the new element
  was the early post-process call that bypassed `skipOverlayDraw`. Reverted to the gated placement + regression
  test. Unproven by GPU capture; the 154811 AV dump (7032) is a game-side fault on another thread, unrelated.

### 2026-10-08 - Generic display gamma correction

- `Graphics.display_gamma`/`gamma_source` implemented in the CAS/RCAS post-process stage (D3D11, native D3D12,
  Vulkan, FSR FG callback output). Details: `post-processing-sharpen.md` "Generic display gamma".
- Closing gate 20261008_172*_build_7034 passes (unit, Python self-tests, 32 FG flow scenarios with `srgb`,
  setup 0.1.7034). Fixed two source-scan tests broken by the change (Vulkan registry key now includes the
  queue; resolved-queue scan window 3000 -> 4000 chars). Hardware run pending.

### 2026-10-08 - Child stop outcomes no longer depend on recording state

- common/ipc owns the local accepted/rejected/acknowledgement-unknown type and stop-command adapter.
  RecordingSession retains a compatibility alias internally; host children no longer import its model.
  No wire/shared-memory or public C layout changes. Existing transport/session/child/IPC tests and
  three syntax-checked product TUs pass; the M3 move mapping now has 8 forbidden edges (was 11).
- The first closing gate's privacy inventory still listed the unstaged deleted header. Staging its
  intended deletion and new IPC headers fixes the inventory without changing privacy checks.
  Resumed closing gate 20261008_140222_build_7032 passes native/Python/privacy/all 32 FG and packages
  the 38,815,644-byte setup PE. No new tests/logging: this preserves classified outcomes and existing
  coverage/diagnostics; real helper finalization/A/V and full runtime composition remain later work.

### 2026-10-08 - Keyboard hook no longer imports controller internals

- Hook startup accepts an immutable host thread/private-message route. Publication precedes thread
  creation; reset follows join. Callback matching and failed-delivery key pass-through stay unchanged.
  Invalid/system-message routes and active retargeting are rejected with startup diagnostics.
- Three native queue interface cases verify message identity/payload, host-selected routing and invalid
  targets without installing a global input hook. Existing matching/ownership/wiring/input tests and
  three product TUs pass. M3's move-time forbidden edges shrink 12 -> 11; DR-19 orders detachment first.
- Closing gate 20261008_133330_build_7031 passes native/Python/all 32 FG checks and packages the
  38,817,040-byte setup PE. Nonthrowing constructor fixes the new static-initialization warning;
  focused 20261008_133841_build_7031 and lint 20261008_133850_build_7031 pass (706 warnings/1014 TUs).
  Three existing-file formatting advisories remain. Real keyboard-hook/app smoke is pending hardware
  validation; no new live-hook regression installs hooks on the user's desktop. Runtime composition,
  remaining controller dependencies, module moves and public API implementation remain M3-M4 work.

### 2026-10-08 - v2 C/C++ ABI baseline verified

- C11/C++20 assert 56 public field offsets/widths and 8 type sizes/alignments; the C object is linked
  and executed, with __cplusplus rejected. C uses ambient pack 1 and restores it; C++ uses default
  packing and verifies move-only wrapper types. Copied-header field drift and removed guards fail.
- DR-18 fixes initializer extent/version: the old pointer-only draft could not safely initialize an
  older client's descriptor after a minor extension. All 30 prototypes match the canonical contract.
  C test flags retain target/debug/CFG/privacy/sanitizers; public/runtime/frontend analyzer scope is covered.
- Interrupted 7028 produced no terminal manifest; stopped processes were confirmed, resume refused.
  Strict-clean 20261008_123915_build_7029 compiled products and passed native/Python/ASan/32 FG;
  assertion macro warnings were fixed, then resumed verify 20261008_125944_build_7029 passed/package:
  38,814,944-byte setup PE; 706 accepted warnings across 1013 TUs. Final new-header formatting and
  20261008_130659_build_7029 focused ABI tests pass. No runtime functions, exports or DLL are added;
  live API/ownership tests, external SDK/MSVC use and hardware/A/V evidence remain later milestones.

### 2026-10-08 - Unshipped v2 API and client ownership wrapper

- Full draft covers lifecycle, commands/events/status, settings and setup/utilities; fixed-width C
  fields and saved/restored 8-byte packing define x64 layouts. C++ wrapper retains event/edit owners,
  preserves errors, bounds waits and rejects embedded NULs. No runtime functions or exports are added.
- Declared Clang compiles the draft as C11 and the wrapper as C++20 with warnings as errors.
  Closing gate 20261008_093153_build_7027 passes native/Python/all 32 FG checks and packages the
  38,815,302-byte setup PE. Permanent layout/C build tests follow; live wrapper/runtime tests are M4.

### 2026-10-08 - Module boundaries enforced by verification

- Lint fails architecture regressions even in advisory mode; unfiltered Python gates run policy/gate
  fixtures, the live tree scan and depth report. Manifest details preserve complete includer counts.
- Clean transaction 20261008_085706_build_7026 exposed fixtures inheriting the enclosing Git approval
  set. The checker now requires the exact repository root; a focused regression protects isolation.
  Resumed verify 20261008_090850_build_7026 passes native/Python/ASan/all 32 FG and lint ratchets;
  setup is a fresh 38,815,186-byte PE. Accepted warnings remain 706 across 1011 TUs.
- Actual temporary hook include of app/main_internal.h fails; restored source scan passes. Twenty-three
  boundary/gate cases (31 with existing lint cases) pass. Q13 documents 12 M3 move-time couplings;
  no new waiver is added. No runtime/frontend/library binary or hardware/A/V delivery is claimed.

### 2026-10-08 - Library module dependency checker

- The source scanner checks quoted/angle and relative includes, ignores comments/raw fixture strings,
  and records 19 exact legacy exceptions with reasons/milestones. New edges and additions to the
  committed exception set fail; pruning removes only obsolete entries. Sixteen behavioral fixtures pass.
- Initial graph: 3645 edges; DX12 internal header 74 includers (2 outside), media 29 (0 outside).
  Depth estimates are labeled; writable-global analysis stays unclaimed once runtime/frontend exist.
- Closing gate 20261008_084914_build_7025 passes native/Python/all 32 FG checks and produces the
  38,814,900-byte setup PE. Direct Python style/types and 16 focused fixtures pass; build wiring follows.

### 2026-10-08 - Library-first plan adopted

- The committed plan moved to library/; index routes to the canonical architecture/API/milestones.
  D8/D9/D13 are superseded; the unpublished v1 facade is replaced and hotkeys/desktop overlay are
  optional runtime features. Previous execution evidence is preserved in archive-2026-W41c.md.
- Documentation-only adoption: relative links, canonical-copy checks and changelog validation pass;
  no product behavior changes or new hardware/A/V claim. Milestone status lives only in library/README.md.

### 2026-10-08 - Runtime package paths and helper configuration handoff

- WinMain now consumes explicit configuration arguments; workers previously ignored the INI path
  already sent by SpawnChildProcess. Package defaults are executable/module anchored, and native child
  ownership copies an explicit helper executable. ACP paths no longer go through a UTF-8 decoder.
- Path/option tests cover spaces/Unicode, duplicate/empty arguments, relative resolution and delegated
  game arguments. Native renamed-helper probe loads the selected INI from another directory with bounded
  process exit/cleanup; focused IPC/configuration/owner checks pass. All five parser fuzz targets pass
  a bounded 10-second-per-target run; runtime options execute 579,654 units.
- Strict-clean 20261007_205639_build_7024 compiled all products and passed native/Python/ASan/32 FG
  checks. After fixing its unnecessary-copy lint finding, resumed verify 20261008_081425_build_7024
  passes and packages the 38,814,820-byte setup PE. Baseline stays 706 across 1011 TUs; four formatting
  advisories remain. Full library runtime/client delivery and hardware/A/V validation stay open.
- Main's server rule requires linear history. Replaced unpublished 01760a89 with 644219f2; exact tree
  and unfinished path files preserved, backup ref retained, four outgoing commits scanned, dry run passes.
