# llm-wiki Log

### 2026-09-24 - Deferred audit items: D3D9 state blocks, sampler re-arm, multi-swapchain sharpen, ANSI paths, DBCS config

The five items deferred by audits 2/3, fixed in code. Incremental gate green at 0.1.6799; no hardware run.

- **D3D9 "S6" state-block restore** - `cross-api-forced-af.md`: Apply reconciled from pre-Apply logical values and
  undid the block (plus a default-reset on the first Apply without an override, and recording taken as applied).
  Per-block snapshots, Capture + BeginStateBlock hooks, merge-then-reconcile. `dx9_sampler_state.cpp` split 792 ->
  710 + `dx9_sampler_state_blocks.cpp`.
- **Sampler slot drift** - re-armed once, 120 presents after a stable drift, with an inline body hook on d3d9's own
  implementation; the slot is never rewritten and the foreign handler never called.
- **Vulkan sharpen per swapchain** - `post-processing-sharpen.md`: registry keyed by (device, swapchain); rebuild
  and `off` retire states and reap them by zero-timeout fence polls; destroy takes live + retired.
- **ANSI paths** - `configuration.md` inventory. Real breakage found: the crash-dump fallback dir was UTF-8 into
  CreateFileA consumers; the dump helper was never found below non-ACP folders; DXVK/ReShade/Streamline version
  probes read '?' paths; `AnsiCompatiblePath` returned "" with CP_UTF8 as ACP.
- **DBCS config** - verified lossy (932/936/949/950); `config_ini_reader` parses UTF-8 files from bytes, locked by a
  differential test against the real API. UTF-8 BOM no longer hides the first section. Fuzz harness drives the
  reader directly (seed `utf8_dbcs_profile.ini`).

Open: hardware runs - a state-block-heavy D3D9 title with forced AF (and a D3D9 overlay that re-hooks samplers),
a Vulkan title with two swapchains and a live `sharpen` toggle, an install/game below a non-ASCII folder (crash dump,
DXVK detection), a Japanese/Chinese/Korean Windows with a UTF-8 config. D3D6-8 state blocks keep the old
refresh path.

### 2026-09-24 - Third risk audit: ten findings plus audit-2 leftovers

Read-only audit, then targeted fixes (no restructuring). Unit gate green; no hardware run for any of them.

- **HDR/10-bit switch mid-recording truncated the file** (re-open of the same staging path). Now finalizes and
  stops degraded; size changes are letterboxed into the locked geometry - `recording-output-paths.md`.
- **OpenGL overlay state leak** (modern path restored almost nothing; legacy path left client pointers into overlay
  memory) - `overlay-rendering.md`, `gl_overlay_state_policy.h`.
- **System audio never followed a default-device switch; device-less start dropped the source silently** -
  `multi-audio-capture.md`.
- **Persistent encode failure had no stop** - 5 s streak ends the recording degraded (`recording_health.h`).
- **exit(-1) dumped as a crash** (Strange Brigade, session `20260924_073945`): 0xFFFF0001..0xFFFFFFFF are ordinary
  exits (`IsSmallNegativeApplicationExitCode`).
- **DX12 focus-loss fence wait** no longer falls back to INFINITE; **DXGI wrapper destruction callback** lost its
  100x `Sleep(1)` poll; **DX11 overlay** unbinds/restores GS/HS/DS and all viewports.
- **config.ini**: UTF-8 values converted to ACP, reload debounce, Unicode/8.3 install path - `configuration.md`.
- Audit-2 leftovers: cursor container rebuild/resize under the leaf lock; `contentHoleSamples` now reaches
  `[AudioFinalization]` and the degraded completion; checked-in `VK_LAYER_CE_overlay*.json` name the gate.

Open: hardware runs (HDR toggle while recording, window resize while recording, OpenGL title with the overlay,
default-output switch and mic plug-in while recording, a TDR). The audit-2 deferrals (S6 state-block restore, sampler
re-hook on drift, multi-swapchain Vulkan sharpen) were fixed in the entry above.

### 2026-09-24 - Review follow-up to the second risk audit (3cfe8272)

A code review of 3cfe8272 found ten issues; eight changed code, two needed no change. Unit gate
green; no hardware run.

- **Breakpoints are recorded first, with no immediate dump.** The one-dump budget was spent by the
  first handled int3, and that dump also latched `g_DumpSuccessfullyWritten`, so a later real
  crash got no dump (`crash_dump_policy.h`; budget constant removed).
- **Retarget never rolls back onto a dead window** during a live recording
  (`ShouldRollBackFailedRetarget`). A lost source always stops degraded; `kContinueDegraded` is
  removed. The `kStopRecording` selector previously fell into that rollback.
- **Audio:** `EncodeResult::trimmedSamples` reports the part of a chunk the recording-end clamp
  dropped, so a failure later in the same call can't count the clamp as a content hole.
  Silence for a refused chunk after a resampler failure is bounded to the recording end
  (`PlaceRefusedChunkHole`).
- **Video output truth:** a Stop that times out on finalize reports the output as degraded
  (`IsFinalizedOutputDegraded`). An expired output I/O deadline now breaks a write that is
  already blocked, via `CancelSynchronousIo` on the writer thread (it registers its own handle,
  and deadline arm/clear is ordered by `outputIoCancelMutex`). FFmpeg's interrupt callback
  only refuses the next transfer.
- **DirectDraw lock tracking is per lock** (`SurfaceLockKey`: the rect for v4/v7, the returned
  surface pointer for the legacy interface). Several non-overlapping rect locks are legal, so an
  Unlock returns `Deferred` while other locks are still held. Anti-leak rules are kept: an
  overlapping Lock replaces the lock it overlaps, a failed or ambiguous Unlock resolves the whole
  track, and capacity is bounded.
- **Config:** a `capture_method` typo warns once. The `Is*CaptureMethod` predicates never log.
- **No change needed:**
  - Vulkan `listedTarget || hostEligible`: `PopulateWhitelistCache` rewrites the persisted list
    from the same config on every publication, so the list can't be stale while a host runs.
  - The DX12 deferred-signal flush under `dx12_hook_g_OverlayMutex`: no mutex holder waits on the
    game's present thread, and narrowing the lock would reopen the fence-teardown race.

### 2026-09-24 - Second risk audit: output truth, injection paths, FG handoff, legacy/Vulkan/audio faults

Follow-up audit of the remaining weak areas (targeted fixes, no big refactors). Eight areas landed;
five were implemented in parallel per-area git worktrees after in-tree parallel agents stalled, and
integrated by checking out each branch's disjoint file set plus one hand-merge
(`streamline_hook_dlssg.cpp`). Green at 0.1.6791 (gate + lint ratchets); no hardware run yet.

- **Mux/output truth** - `recording-output-paths.md`: committed video now survives every non-empty
  exit path (cancellation or finalize failure publishes instead of deleting; only zero-frame
  outputs are removed); a failing HDR metadata packet drops only that packet; local outputs got
  the stream-side 30 s I/O deadline; the first lost write stops the recording; and
  `MediaEngine_WasLastOutputDegraded` latches the `RecordingSavedDegraded` health flag.
- **Child injection / entry patches**: `ChildInjectRequest` carries the DLL path in the request
  buffer (remote `LoadLibraryW`, wait result and exit code checked before logging "Injected");
  new `child_inject_policy.h` + `hook_jump_policy.h` (x86 rel32 wrap semantics, x64 fails closed);
  entry-patch failures are collected and logged only after peer threads resume - never while
  suspended. `inline_hook_deep.cpp` split 804 -> 745 lines into `inline_hook_pristine_image.cpp`.
- **DX12 FG handoff** - `frame-generation/guardrails.md`: the fresh-Streamline-handoff exemption
  now covers both deferral gates (the menu-switch FSR -> DLSS case that stranded the overlay);
  the four keep-alive exceptions share `keepOverlayLiveAcrossOuterOff`; GetState-only suppression
  retires on generation evidence; the fence flush runs under the overlay mutex with the swapchain
  pinned; Smooth Motion interposer pass-through fails closed (no overlay draw).
- **Source-loss retargeting**: `capture_retarget_policy.h` centralizes it - window/explicit targets
  stop the recording instead of silently switching to auto-monitor, latched source-loss health is
  published before finalization, window targets pin their monitor by stable ID, SDR white nits are
  queried per monitor, and a source-format-vs-HDR-contract contradiction forces an urgent recheck.
- **Legacy D3D/DDraw**: overlay composite for 8-bit palette and 24-bit DirectDraw fullscreen (New);
  DX8/DX9/OpenGL present re-entry guard (a co-resident overlay re-entered Presents until the stack
  overflowed); the D3D7 font-atlas lock takes `DDLOCK_NOSYSLOCK` (Gothic II hang); one failed
  DirectDraw Unlock no longer stalls DirectScanout and the freeze watchdog. Sampler-hook drift is
  detected per Present. Deferred: S6 state-block restore (`cross-api-forced-af.md` stale-risk) and
  sampler-slot re-hook on drift.
- **Vulkan layer** - `dx12-injection-bootstrap.md`: every queue submit holds
  `ScopedBorrowedQueueSubmission` (single-queue/async-present device-lost suspect); layer
  participation is host-eligible OR listedTarget (no session-long lockout) with UTF-16 matching
  end to end; the fallback install path generates the gate manifest (never the full layer);
  sharpen only rides the first live swapchain >= 320x180; the submit-thread heuristic needs a
  mismatch within the same 2 s window as the acquire route. Registration split 802 -> 647 lines.
  Deferred: simultaneous multi-swapchain sharpening; checked-in `VK_LAYER_CE_overlay*.json` still
  names the full layer.
- **Audio fault accounting** - `recording-output-paths.md`: the encoder FIFO grows-or-refuses whole
  batches (dead five-second ceiling removed; nothing truncates); `avcodec_send_frame` failures
  retry on EAGAIN and place losses as explicit holes so PTS stays strictly monotonic; consumed
  ranges are never re-filled (this silently compressed tracks before); cross-thread cursor reads
  snapshot under `g_audioCursorSyncMutex`, a strict leaf lock. Deferred: `contentHoleSamples` has
  no consumer yet (worth adding to `[AudioFinalization]`); same-class unlocked cursor reads remain
  in `mediaengine_recording_stop.cpp` / `mediaengine_timeline.cpp`; no mock seam for real
  codec/FIFO-allocation failures (covered by source-policy pins instead).
- **Common/crash/config**: invalid config boundary values warn instead of silently resetting;
  crash traces redact the account path component; quick-assert dumps are capped (3 per process,
  breakpoint path 1); the shared-memory ABI fingerprint covers nested struct sizes and the
  DLSS-FG/sharpen field offsets (still version 63); >= 50 ms calls get a slow-call notice.
- **Lint ratchets**: three new-code findings fixed (FARPROC cast, stale parameter comments, a
  `memcmp` over `CONTEXT`); the residual increases are header re-emission from the new translation
  units plus the documented analyzer null-guard false-positive class on `present_reentry_guard.h`,
  folded into `clang_tidy_baseline.json` deliberately after a full product build.

### 2026-09-24 - FSR FG -> DLSS FG: overlay init stranded on the fresh Streamline queue

Talos `20260923_233317` (0.1.6782): after the menu switch, `sl.dlss_g` recreated the swapchain on a new queue
(`fgOwned=1`), DLSS-G never interpolated before the user quit (fence frozen at 424, no `SetOptions(ON)`), and
`Deferring inactive runtime-owned swapchain overlay init until queue settles` repeated until exit. Fixed by the
fresh-handoff exemption in `streamline_ownership.h` (see `frame-generation/guardrails.md`). Open: hardware
re-test; whether the pre-SL scQueue draw then hands over to PostSL cleanly once DLSS-G starts after a menu switch.

### 2026-09-23 - Risk audit: ten user-facing hazards addressed

A read-only audit asked which areas were most likely to hurt users; it led to the local
changes below (no restructuring). No hardware run yet for any of them.

- **Crash VEH dumped handled first-chance faults** (Mono/JVM/.NET/LuaJIT/emulators): now
  recorded only, dumped when the process dies of them - `regression-testing-and-logging.md`.
- **A failed trailer/close deleted whole recordings** - `recording-output-paths.md`.
- **One late `ReloadConfig` ack killed the media child mid-recording** - `process-ipc.md`.
- **Media `ReloadConfig` raced recording threads on `config`**: parked in `deferredConfig`,
  assigned at the next `StartRecording` with the old assign-only semantics.
- **Vulkan implicit layer sat in every Vulkan process** as passthrough: now declines at
  negotiation in non-targets when a compatible host is present. The no-host target list
  first had a location mismatch (written beside `captureengine.exe`, read beside the staged
  layer DLL); it now lives in `HKCU\Software\CaptureEngine\VulkanLayerTargets` -
  `dx12-injection-bootstrap.md`. The loader's documented
  behavior for `VK_ERROR_INITIALIZATION_FAILED` from negotiation is "unusable, not loaded"
  (confirmed in Vulkan-Loader `loader_create_instance_chain`), but it still MAPS the
  library first, so the manifest now names a negotiation-only gate DLL that loads the full
  layer only for admitted processes. `--verify-runtime` gained a deterministic
  participation probe (`testapp/run_vulkan_layer_participation.py`), not yet run.
- **Steam null-callback VEH** acted on other threads' faults and wrote a fixed Steam RVA
  (0x1621d8, stale since at least the RoboCop build's 0x167340); now thread-scoped, proven
  slot only, registered once instead of add/remove per Present.
- **FFX `ffxConfigure` int3 race**: a second thread trapping after disarm escaped to the crash
  handler; `ClassifyEntryBreakpoint` resumes it. Armed flag is published before the byte.
  A byte that cannot be restored no longer spins the thread.
- **Injection used ANSI paths** (`LoadLibraryA`): UTF-16 end to end now. Other hook-side
  `GetModuleFileNameA` users (logs/config dirs, ~56 files) remain ANSI - stale-risk for
  non-ACP install paths.
- **~180 MB symbol copy per session, twice**: hard links into `logs\symbol_store`
  (`common/crash_symbol_store.h`), pruned by link count; the hook never archives.
- **Freeze dumps** prefer the registered external helper when available. Audit correction: the effective
  watchdog timeout is 30 s (120 s UE5/DLSS-FG), not the 5 s field initializer.

Open: `log_level=trace` stays the template default (diagnostics-first project policy);
hardware validation of all of the above; a hardware-fault test for the
KiUserExceptionDispatcher range (only the RtlRaiseException path is unit-tested);
a hardware run of a whitelisted Vulkan title started before CaptureEngine (late wake
through the registry-persisted target list).

### 2026-09-23 - Vulkan sharpen lost the device on DOOM Eternal's swapchain recreate

Session `20260922_235937`, build 0.1.6773, first run with `sharpen=cas` active at Vulkan
startup: the sharpen pass ran on the startup swapchain, DOOM destroyed it (no `oldSwapchain`)
and NVIDIA returned the same handle for the replacement. The sharpen state was never released
on destroy, passed the present-time generation check, and drew through freed image views:
`QueueSubmit FAILED with result -4`, black window. The manual dump shows only the game's
threads parked, with no CE frame, as expected for a GPU-side fault. This repeats both DOOM overlay
lifetime bugs (`20260913_174040`, `20260914_122133`) in a component added later. Fixed per
`post-processing-sharpen.md` "Vulkan swapchain lifetime". The same session's earlier DOOM run
shows the second bug: `sharpen=cas` published live twice, and the layer never read it. Hardware run
pending: start DOOM with sharpen on, toggle it live, change resolution/fullscreen.

Follow-up session `20260923_003755` (0.1.6774): the recreate was clean, then DOOM moved its present
to compute-only family 2 on the live swapchain and the render-pass route kept submitting there.
The device was lost 2.3 s later. Sharpen now has a compute route and rebuilds on a family change
(`post-processing-sharpen.md` "Vulkan compute route").
