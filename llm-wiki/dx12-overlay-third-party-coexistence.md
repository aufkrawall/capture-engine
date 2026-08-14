# DX12 Overlay Third-Party Coexistence

Last cross-checked: 2026-08-14 (Present-entry rule generalized to draw order in 0.1.5960: CE stays out of *any* `dxgi!Present` entry a foreign overlay owns — not only one two or more share — and intercepts below it with a deep hook in the `dxgi!Present` body, so its overlay composites after Steam's and RTSS's instead of underneath them; the FG-interposer exception is gone because the body view supersedes it, the prepend survives only as the refusal fallback against a single overlay, Streamline present routing is refused below the chain, and the foreign-jump classification now resolves the Present-owning module from the address instead of by name. Generic hook-chain ownership, thread-quiesced patching, proxy-module exclusions, and resident pass-through lifecycle remain as audited on 2026-08-11; the prior DX12/FG provenance rules remain in force. Open: layering against object-wrapping proxies such as ReShade — see Known limitations. The native-FSR-FG below-the-chain and marker-only topmost handoffs are covered in their own resolved sections below.)

Primary sources:
- `hook/common/overlay_compat.h`
- `hook/common/overlay_compat_detail/module_table.h`
- `hook/common/dx12_overlay_policy.h`
- `hook/apis/dx12_hook_main.cpp`
- `hook/apis/ffx_hook.cpp`
- `hook/main.cpp`
- `hook/main_host_lifecycle.cpp`
- `hook/main_overlay_detect.cpp`
- `hook/wrappers/inline_hook.cpp`
- `hook/wrappers/hook_patch_transaction.cpp`
- `hook/wrappers/iat_hook.cpp`
- `hook/wrappers/vtable_hook.cpp`
- `hook/common/dxgi_shared.cpp`
- `hook/common/dxgi_shared_present_core.cpp`
- `hook/common/dxgi_shared_original.cpp`
- `hook/common/dxgi_shared_steam.cpp`
- `hook/common/dxgi_shared_detail/types_and_state.h`
- `hook/apis/dx12_hook_process.cpp`
- `hook/apis/dx12_hook_process_session_phase2.cpp`
- `tests/test_dxgi_shared.cpp`
- `tests/test_dxgi_shared_part8.cpp`
- `tests/test_dxgi_shared_part10.cpp`
- `tests/test_dxgi_shared_part11.cpp`
- `tests/test_fps_limiter.cpp`
- `installed/captureengine/logs/20260602_215620`
- `installed/captureengine/logs/20260602_215334`
- `installed/captureengine/logs/20260602_213952`
- `installed/captureengine/logs/20260602_161416`
- `installed/captureengine/logs/20260602_030350`
- `installed/captureengine/logs/20260601_212556`
- `installed/captureengine/logs/20260531_232108/hook_debug.log`
- `installed/captureengine/logs/20260531_230835_talosfsrfg/hook_debug.log`
- `installed/captureengine/logs/20260809_015416`

## Scope
This page records the current repo knowledge for making our overlay and capture hooks coexist with external injects and overlays. The detailed historical cases are DX12-heavy, while the generic ownership and lifecycle invariants also cover DXGI, D3D8/9/11/12, DDraw, OpenGL, and Vulkan.

## Cross-API Hook-Chain Invariants (2026-08-11)

- Generic module identity tracking recognizes ReShade, Special K, OptiScaler, RTSS, Steam, Rockstar, EOS, Discord, Overwolf, and the established FFX/Streamline modules. Identification is refreshed off the Present thread from module paths, exports, and version metadata; the hot path reads only the published atomic registry.
- CE prepends its inline detour at the original function entry when it finds a foreign relative `E9` jump or the x64 indirect `FF 25` entry shape used by common custom hooks and Microsoft Detours. On x64 it replaces only the five-byte foreign entry with an `E9` to a nearby CE relay; the saved predecessor trampoline enters the exact foreign target, and bytes from `target+5` onward remain untouched so an RTSS/Detours trampoline that resumes there stays valid. This preserves the existing chain instead of patching inside another tool's trampoline or bypassing it.
- Inline and deep-hook byte writes are process-wide transactions: peer threads are suspended, their instruction pointers are proven outside the patch range, expected bytes are revalidated, and only then is the entry changed. Installation fails closed if the process cannot be quiesced. The old INT3 transition window is forbidden.
- Removal is ownership-based. Inline/deep hooks restore bytes only when the live bytes are still CE's; IAT and vtable hooks use compare/exchange against the exact CE detour. If a later tool followed or replaced CE, removal preserves that foreign entry and CE's saved chain/trampoline remains resident so the later hook never calls freed code.
- Graphics proxy DLLs (`dxgi`, D3D, OpenGL and common input/audio proxy names used by ReShade/Special K/OptiScaler) are excluded from CE's broad IAT patch scan. Their own imports and internal dispatch remain under the proxy's control; CE reaches the application/system entry chain instead.
- Host shutdown is a dormant pass-through transition rather than a physical DLL unload. DirectX/OpenGL routes perform no CE work; Vulkan likewise disables CE rendering/capture/overrides but retains the minimum dispatch, queue, and swapchain metadata needed to forward correctly and reactivate. This keeps wrapper vtables, callbacks, trampolines, and foreign saved predecessors callable while the game continues, then permits target-specific reactivation by a later CaptureEngine host.
- Every API detour must test CE runtime eligibility before overlay, capture, screenshot, limiter, override, or mutable state work. Dormant calls forward through the exact predecessor. This rule applies equally to Present/Present1, device wrappers, state/sampler detours, OpenGL swaps/context deletion, and Vulkan queue presentation.
- Third-party overlay inclusion in a capture is **best effort and order-dependent**. If the foreign overlay draws before CE's capture point, it is included; if it draws later, forcing an extra invocation or reordering its private GPU work is unsafe. Coexistence and visibility take priority: CE must preserve the natural chain and must not hide or disable either overlay merely to force it into the recording.

## RESOLVED: CE must stay out of a Present entry a foreign overlay owns (2026-08-12, builds 0.1.5934 / 0.1.5960)

### The invariant

**CE does not join a `dxgi!Present` entry chain a foreign overlay owns. It intercepts BELOW it, with a deep hook in the function body.**

Two independent reasons, either sufficient on its own:

1. **Chain integrity** (0.1.5934, the original rule, applies from two overlays upwards) — the saved-chain corruption described below.
2. **Draw order** (0.1.5960, applies from one overlay upwards) — every participant in that chain composites *before* it forwards, so the participant that runs **last** ends up **on top**. CE's prepend made CE the first participant and therefore the bottom layer: Steam's fullscreen overlay and RTSS's OSD drew over CE's overlay (user report, build 0.1.5959). Below the chain CE composites after all of them, which is what the project rule "CE's overlay is topmost" requires. `NoteOverlayCompositeSite` states the resulting layering once per edge: `[OVERLAY LAYER] CE composites BELOW the foreign Present chain … CE's overlay is the topmost layer` vs `… ABOVE the foreign Present chain … appears ON TOP of CE's overlay`.

Reason 1 alone would still permit the prepend against a single overlay, so it is what the **fallback** rests on: if the body patch is refused (thread quiescence, an unrecognized prolog, a 32-bit target), CE falls back to the prepend *only* with fewer than two overlays — losing draw order there is far better than running with no Present view at all, and the chain still composes. With two or more, the prepend stays forbidden and the body patch is retried on the next real swapchain event.

The chain-integrity half of the invariant, unchanged:

Steam and RTSS both implement their DXGI Present hook as *save the current entry bytes, patch, and on every call restore the saved bytes, `call [entry]`, re-patch* — all on the same shared five bytes. Two such tools compose naturally (the later hooker's "next" is the earlier one). A third participant does not: whichever tool (re-)installs its hook while CE's prepend is live records **CE's relay** as its own "next", which silently drops the other overlay out of the chain. The corruption lives in the foreign tools' saved-chain state, so **no forwarding choice on CE's side can repair it** — see the four failed attempts below.

### Evidence

- Ground truth is the caller-attributed ECL trail (`ce_dx12_trace`). Frame 1 of sessions `installed/captureengine/logs/20260812_002958` and `20260812_010529`: `capture_hook_x64.dll > gameoverlayrenderer64.dll > RTSSHooks64.dll` — game -> CE -> Steam -> RTSS -> real Present, **all three overlays drawing**. From frame 2 onward Steam's saved "next" no longer reaches RTSS: RTSS submits exactly 1 ECL for the whole session while Steam submits 98/269/349.
- RTSS's handler is `RTSSHooks64+0x72F20` (7.3.6, disassembled): reentrancy `lock bts`, optional save of the live entry bytes, `rep movsb` restore of its install-time bytes into `[hookedFn]`, `call [hookedFn]`, then a `rep movsb` re-patch with either the bytes it found live or its own hook bytes.
- Steam's hook installer is `gameoverlayrenderer64+0x8da00`; its call sites (`+0x64a92`, `+0x64ae6`, …) pass `&origSlot` in `r8` and test `cmpq $0, origSlot` immediately after, i.e. the slot is the saved "next" that CE's prepend poisons.
- With one foreign overlay this never bites: Talos/GTA/RoboCop show seed bits `0x1000001` (`20260811_214252`) = Steam + `sl.interposer` only, and `sl.interposer` is not in the overlay subset.

### Current fix

- `ce::overlay_compat::CountLoadedTrackedOverlayModules(TrackedOverlaySubset::kOverlay)` counts the loaded overlay subset (loader-free, off the seed/notification cache). `ShouldLeavePresentEntryToForeignOverlayChain(foreignEntryPatchOwnedByOverlay, loadedOverlayCount)` is the decision: **at least one** loaded overlay with the entry patch attributable to it (`externalJmpDetected || loadedOverlayCount >= 2` — with two the count is the evidence because the bytes flicker, with one a visible patch is required since an unpatched entry has no chain to go below). `MayPrependPresentEntryWhenBelowChainViewUnavailable(loadedOverlayCount)` is the refusal fallback (`< 2`). `ShouldLeavePresentEntryAfterRuntimeSwapchainWrap(entryPatchStillIntact, ...)` gates the ownership-checked un-prepend once the Streamline runtime swapchain has been wrapped; that path is now only reachable when the install-time body patch was refused, since the install-time decision otherwise already left the entry.
- **The FG-interposer exception is gone (0.1.5960).** It existed because the alternative to the prepend used to be wrapper-only interception, which cannot see a runtime present on a swapchain CE never created. The alternative is now the deep body hook, a full-strength Present view that covers *more* than the entry hook did (including swapchains that pre-date injection), so the exception had no remaining purpose and only kept FG games on the bottom layer. Consequence: Talos/GTA with DLSS or FSR FG now converge on the same below-the-chain topology the `dx12_fg_switch_test` matrix was validated on in 0.1.5957.
- **Streamline present routing is refused below the chain** (`DetectSLPresentHook`). There `oPresent` *is* the live foreign entry, and the SL route forwards through it directly rather than through `CallOriginalPresent` (the only place that prefers the deep trampoline), so activating it would re-run Steam/RTSS and re-enter CE's own body hook — unbounded recursion. Below the chain SL has already run by construction, so there is nothing to route into. This was previously unreachable only by accident (the mode leaves `oPresentTrampoline` null); with FG interposers now allowed into the mode it is refused outright.
- When it holds, `InstallPresentInlineHooks` creates the bypass trampolines and records `g_externalOverlayPresentHook` for diagnostics, then installs **no entry patch at all**, publishes `oPresent`/`oPresent1` as the live entry addresses, and sets `dxgi_shared_s_presentEntryLeftToForeignChain`.
- `ShouldInstallSwapchainHooksWithThirdPartyOverlay(..., presentEntryLeftToForeignChain)` then also keeps the swapchain class vftable pristine — Steam resolves its own "next Present" from `vtable[8]`, so claiming that slot would put CE straight back into the chain the mode exists to leave.
- Without a Present view of its own, `HasPresentDetourHooks()` returns false, `ShouldDelegateDX12PresentToDetourHook` stops delegating, and `CWrapDXGISwapChain::Present` does the overlay/capture work itself and forwards through `m_pReal->Present`. The foreign chain is then byte-identical to a process without CE. **That wrapper-only state is a fallback, not the normal one — see the deep body view below.**
- `CallOriginalPresent` / `CallOriginalPresent1` forward through the live entry in that mode — never a trampoline, a saved foreign target, or the DXGI bypass, each of which drops one overlay. Log markers: `InstallPresentInlineHooks: N third-party overlays already share the Present entry ... CE stays out of the entry patch chain`, `CallOriginalPresent: foreign-chain entry forward #N`.

### The wrapper alone is not a view: CE also hooks the Present BODY below the chain (2026-08-12)

**Validated** (`installed/captureengine/logs/20260812_153302`, build 0.1.5957, user-confirmed): dx12_fg_switch_test launched from Steam with RTSS active, full matrix Off -> DLSS FG -> Off -> FSR FG -> Off. CE's overlay visible throughout alongside Steam's and RTSS's, one 26.4 ms `TOTAL SLOW` in 6466 presents, no errors. Known residual: the FSR-FG -> Off edge recreates the swapchain and leaves ~484 ms / 61 presents uncovered (`gate=overlay-backend-uninitialized`); the DLSS-FG edge is clean (`uncovered=0`). See `log/recent.md`.

**Symptom** (`installed/captureengine/logs/20260812_140930`, `dx12_fg_switch_test` launched from Steam, all FG off, Steam overlay + RTSS both loaded, build 0.1.5950): no CE overlay at all for the whole session. The game rendered fine (`ECL timing/1s: count=720`), CE saw zero presents.

**Why the leave-entry mode was blind.** Its premise — "intercept through `CWrapDXGISwapChain`" — only holds for swapchains CE itself created. Here CE injected *after* the game already had its D3D12 device and swapchain: WMI's process-start notification arrived at `14:09:52.218`, injection completed at `52.428`, and the log shows `deviceCreated=0`, no `CreateSwapChainForHwnd` interception for any real swapchain, and `DX12: Postponed temp swapchain also failed — pre-existing swapchains will not have overlay`. A swapchain the game already holds cannot be wrapped retroactively, and reducing WMI latency can never close that race for good. So the mode has to work without a wrapper.

**Fix: a deep hook in the `dxgi!Present` body, at the verified resume offset past the foreign five-byte entry patch.** Steam and RTSS both only save/restore/re-patch those five entry bytes, and their trampolines resume exactly at that offset, so CE's body patch is invisible to them, cannot be clobbered by their re-hook cycles, and is not part of anyone's saved chain. Order becomes game → Steam → RTSS → **CE** → real body: all three overlays draw, CE composites last, and the entry bytes stay entirely foreign.

- `InstallPresentBodyHooksBelowForeignChain` (`hook/common/dxgi_shared_hooks_present.cpp`) runs inside the leave-entry branch and covers `Present` and `Present1` alike.

**The entry bytes are volatile — one sample is not evidence.** RTSS restores the original entry bytes, calls through, and re-patches on every present, so byte 0 reads clean roughly half the time on an entry that is very much hooked. Session `installed/captureengine/logs/20260812_150918` holds the contradiction three log lines apart: `2 third-party overlays already share the Present entry (E9 at 00007FFD5C049960 …)`, then `DeepHook: No external hook at byte 0 of 00007FFD5C049960 (byte=0x48)` — 0x48 being the original first byte — then `deep body hook … FAILED`. `Present1` installed, `Present` did not, and `Present` is the entry the game uses, so CE was blind again.

  - `InstallDeepHook`/`InstallDeepHookPublished` take `minimumExternalPatchSize`. The caller passes the span it observed and the body patch is placed past it whatever byte 0 reads now; a visible span narrower than the observed one is widened, never narrowed. Too large is safe (the foreign trampoline resumes below CE's patch and still reaches it), too small never is, so an unobservable patch uses the widest form CE recognizes (14, the `FF25` shape). With no observation at all the strict refusal stays.
  - The leave-the-entry decision moved **ahead of** the bypass machinery (which does need a visible jump) and rests on `externalJmpDetected || loadedOverlayCount >= 2`. The module count does not flicker; the entry bytes do. This also closes the mirror race: a sample taken inside RTSS's restore window would otherwise have made CE prepend into a chain two overlays share — the exact state this mode exists to avoid, previously decided by a coin flip.
- The deep trampolines are published as `dxgi_shared_oPresentDeepBody` / `dxgi_shared_oPresent1DeepBody`. `CallOriginalPresent`/`CallOriginalPresent1` check them **before** every foreign-chain entry forward: control arrived from below the chain, so forwarding through the live entry would re-run Steam/RTSS and re-enter the deep hook without end. Marker: `CallOriginalPresent: foreign-chain deep body forward #N`.
- They also replace `dxgi_shared_oPresentBypass` / `oPresent1Bypass`. The DXGI bypass resumes at exactly the offset the deep hook now owns, so every "skip the foreign entry hook" consumer would otherwise land back in CE's own detour; the deep trampoline skips the foreign entry *and* CE's patch.
- `HasPresentInlineHooks()` / `HasPresentDetourHooks()` count the deep bodies — otherwise `FindAndWrapPreExistingSwapchains` keeps reporting failure and the wrapper keeps running a second Present path over the same frames. `HasPrependedPresentEntryHook()` is the new predicate for "CE owns entry bytes"; the late-overlay-join warning in `main_overlay_detect.cpp` uses it, because in this mode CE owns none.
- **Complete deep coverage also preserves swapchain COM identity (2026-08-14).** Leaving the shared entry and class
  vtable pristine is insufficient if CE then hands the game/runtime a `CWrapDXGISwapChain`: that proxy changes the
  object and mirrored AddRef/Release traffic observed by Steam/RTSS, while its Present delegation creates a redundant
  second CE transport. Session `20260814_004913` ended with four foreign refs after wrapper teardown, then official
  FFX replacement creation failed `E_ACCESSDENIED`; RTSS submissions also stopped while Steam continued. When at
  least two tracked overlays are loaded and both Present methods have deep hooks, all ordinary DX12 factory paths now
  return the real swapchain. The wrapper remains only where it supplies missing coverage (incomplete deep view,
  non-DX12/single-overlay fallback, or the dedicated non-retaining Streamline runtime route).
- Internal D3D10/11 hook-discovery swapchains are thread-locally excluded from the DX12 global factory detour. RTSS's
  D3D11On12 startup had caused CE's temp D3D11 chain to enter DX12 wrapping/tracking and retain a foreign reference.
  The thread-local scope preserves concurrent real game creates, while first proven D3D12 Present publishes API-use
  evidence before HookThread can start the DLL-only D3D11 fallback.
- Only an obtained view latches `s_inlineHooksInstalled`. Thread quiescence may legitimately refuse a body patch once (a peer thread inside the displaced range); the next real swapchain event then retries through `EnsurePresentInlineHooksForRealSwapchain`. No timer, no sleep.
- `InlineHook::InstallDeepHook` previously required an all-`PUSH` prolog, which rules out `dxgi!CDXGISwapChain::Present` — it opens with `48 89 5C 24 10` (`mov [rsp+10h], rbx`), a shadow-space save. `ce::inline_hook_policy::TryComputeDeepHookPrologStackDelta` now accepts `PUSH r64`, `sub rsp, imm8/imm32`, and shadow-space saves (RSP delta 0), and refuses everything else; the patch omits the `add rsp` entirely when the delta is 0. Present resolves to resume offset 5, 0 bytes of undo, 14 displaced bytes.
- Deliberately unchanged: with an FG interposer loaded, `ShouldLeavePresentEntryToForeignOverlayChain` is still false at install time, so the Talos DLSS-FG prepend-then-un-prepend path takes no deep hook and is byte-identical to 0.1.5946.
- Regression tests: `tests/test_dxgi_shared_part14.cpp` (prolog stack-delta shapes, accepted and refused), `tests/test_dxgi_shared_part13.cpp` (the body view is taken in the leave-entry branch, latches only on success, republishes the bypass; the deep forward precedes every entry forward in both `Present` and `Present1`; the two predicates).

**Invariant that comes with it: below a foreign chain, the immediate caller of `dxgi!Present` proves nothing about who originated the present.** It is always the last foreign overlay in the chain, for every swapchain including the game's own. Session `installed/captureengine/logs/20260812_144425` (0.1.5953, the deep view's first run) showed the overlay rendering normally and then vanishing at the exact frame the deep hook took over: every present logged `DetourPresent: Bypassing DX12 ProcessFrame for third-party overlay swapchain 0000021DFC5FEFE0 (caller=…\RTSSHooks64.dll)` — the game's own swapchain, one line per 256 presents at full frame rate.

- `CapturePresentCallContext` (and the equivalent block in `DetourPresent1`) now resolves provenance according to the interception point, gated on `DXGIShared::IsPresentInterceptedBelowForeignChain()` (derived from the deep-body trampolines, so it cannot drift):
  - `callerFromThirdPartyOverlay` is **suppressed** below the chain — the inference is structurally false there. `DX12_IsThirdPartyOverlaySwapchain` (swapchain identity, recorded at creation) stays authoritative, and `ShouldKeepStartupBlockingOverlaySwapchainBypass` still works off it.
  - `callerFromStreamlineModule` / `callerFromFFXFrameGenerationModule` are **recovered from the stack** (`HasStreamlineModuleInCurrentStack()`, `ce::overlay_compat::HasFFXFrameGenerationModuleInStack()`) instead of dropped: the interposer frame is simply a few frames further out, and losing it would misroute every FG present.
- Regression test: `tests/test_dxgi_shared_part13.cpp` (`PresentProvenanceIsNotTakenFromTheImmediateCallerBelowAForeignChain`).

**Second invariant: below the chain, CE must never invoke a foreign overlay handler.** Every overlay in the chain has already drawn on the way down to CE. Session `installed/captureengine/logs/20260812_145524` (0.1.5954, DLSS FG on) measured `DetourPresent TOTAL SLOW 5014.9ms` per present — about 0.2 fps — and its manual dump caught the exact loop on the `sl_dlss_g` worker:

```
sl_dlss_g -> sl_common -> capture_hook!CWrapDXGISwapChain::Present
          -> gameoverlayrenderer64!OverlayHookD3D3 -> RTSSHooks64
          -> capture_hook!DXGIShared::DetourPresent            (deep body hook)
          -> TryInvokeGuardedExternalSteamOverlayPresent
          -> gameoverlayrenderer64!OverlayHookD3D3 -> RTSSHooks64 -> kernel32!GetTickCount
```

CE re-invited a chain that had already run above it; RTSS hit its own reentrancy guard and spun. `TryInvokeGuardedExternalSteamOverlayPresent` now fails closed on `IsPresentInterceptedBelowForeignChain()` **before** it resolves the foreign handler, so the caller falls back to its own forward (the deep trampoline). `AttemptSteamDX12OverlayInit` needs `dxgi_shared_s_hookedVTable`, which stays null in this mode, so it was already unreachable.

Note what enabled it: recovering `callerFromStreamlineModule` unlocked the `callerFromStreamlineModule && !s_slRoutingActive && steamOverlayLoaded` external-overlay transport. The provenance recovery is right; inviting Steam from below the chain never is.

**Hot-path cost rule for that provenance walk.** Address→module resolution takes the loader lock, so the recovery is a single bounded walk (`ResolvePresentOriginatorBelowForeignChain`, `hook/common/dxgi_shared_present.cpp`) that steps over CE's own frames, the tracked foreign overlays, and DXGI/D3D dispatch, then classifies the **first** real originator and stops — typically three to five resolutions, one pass answering both FG questions. Never a full-stack scan per module (`HasStreamlineModuleInCurrentStack` + `HasFFXFrameGenerationModuleInStack` would be up to 40 loader-lock resolutions per present). Regression test: `NoForeignOverlayHandlerIsInvokedWhileCEInterceptsBelowTheChain` and the resolver assertions in `PresentProvenanceIsNotTakenFromTheImmediateCallerBelowAForeignChain`.
- Superseded by 0.1.5960 for the "single foreign overlay keeps the prepend" half: a single overlay now also goes below the chain (draw order), with the prepend kept only as the refusal fallback. The rest of this bullet stands. With a loaded FG interposer, CE wraps the Streamline runtime-owned swapchain with the **non-retaining wrapper** (0.1.5946) and then removes its own entry prepend ownership-checked, so the entry is left to the foreign chain in FG games too. The non-retaining wrapper borrows the runtime's CreateSwapChain reference and mirrors no refs, so Streamline's release/recreate on FG transitions is byte-identical to a process without CE (a retaining wrapper pins the old swapchain and breaks the DLSS-G handoff with `E_ACCESSDENIED`). While CE still owns the entry it is a pure passthrough (delegates to the detour hook); in leave-entry mode it drives ProcessFrame plus the gated PostSL callback (confirmed-standalone AND unconfirmed startup family) and feeds `g_PresentCallCounter` so the Streamline present-stall detector cannot false-positive. Log markers: `Wrapped Streamline runtime swapchain`, `CE left the Present entry to the foreign overlay chain`, `DetourPresent(wrapper): Invoking PostSL on wrapped Streamline runtime Present`.
- Regression tests: `tests/test_overlay_compat.cpp` (overlay-subset counting excludes `sl.interposer`/render-only modules; the entry-chain policy matrix), `tests/test_dxgi_shared.cpp` (`SwapchainVTableStaysPristineWhileTheForeignPresentChainOwnsTheEntry`), `tests/test_dxgi_shared_part11.cpp` (install skips the prepend before `InstallPublished`, forwards run the live entry first, and **no tool-specific handler resolution may return**).

### Never enter a foreign overlay handler that is not initialized

A NULL Present-shaped callback slot in `gameoverlayrenderer64.dll` means Steam's own overlay hook has not finished installing. Entering its handler then dispatches through an uninitialized pointer. Talos + DLSS FG + RTSS crashed that way twice (`installed/captureengine/logs/20260812_024730` and `20260812_030202`): `0xC0000005` DEP execute violation on a heap address, RAX=0, with `steamCallback=0000000000000000` logged on the very invoke that faulted and **zero** `gameoverlayrenderer64` command-list submissions in the session — Steam was never drawing in that state.

`ShouldInvokeGuardedExternalSteamOverlayPresentForCallbackState` used to return `steamNullCallbackRecoveryAvailable` for a NULL slot, i.e. "enter anyway, the crash-time VEH will catch it". It does not: `SteamOverlayInitVehHandler` only recognizes the `call rax` / RIP=0 shape, while this fault is a jump to a garbage pointer several frames deep inside Steam. A NULL slot now fails closed to CE's clean DXGI bypass. This is self-correcting — Steam still initializes on its natural path (it owns the entry after its own re-hook), and CE resumes servicing it as soon as the slot reads like a real renderer.

**Invariant: control-flow decisions must not be delegated to an exception handler.** A guard that says "enter and rely on the VEH" is a bug, not a safety net. The backstop that remains is genuinely last-resort: inside a CE guarded foreign invoke, an execute violation on a non-executable address whose pushed return address is CE code resumes at that return address with `DXGI_ERROR_INVALID_CALL`, so the guarded-invoke fallback presents through the bypass instead of killing the process.

Note this also explains the removed proactive slot patch: it *was* masking these crashes, by filling the NULL slots. It did so at the cost of severing Steam's chain to every overlay below it, which is why declining the invoke - not writing Steam's state - is the correct trade.

### A saved foreign Present handler is never valid forever

`dxgi_shared_g_externalOverlayPresentHook` is captured once, at install. The overlays that share the entry rebuild their hooks on their own schedule (RTSS restores and re-patches on every call; Steam re-hooks on new swapchains), which frees or rewrites the runtime-allocated thunk CE recorded. Talos `installed/captureengine/logs/20260812_024730` crashed exactly there: five milliseconds after `foreign re-hook took the Present entry from CE`, CE jumped through that thunk whose `FF 25` payload now read `0x295C8999101` — a heap address — and died with a DEP execute violation (`0xC0000005`, RAX=0) inside `TryInvokeGuardedExternalSteamOverlayPresent`.

**Invariant: never transfer control to a saved foreign handler without proving it still points at code.** `IsCallableForeignPresentHandler` requires the entry, and for an `E9`/`FF25` thunk the address it forwards to, to be committed executable memory. `GetCallableExternalOverlayPresentHook` validates and, when stale, `RefreshExternalOverlayPresentHookFromLiveEntry` re-derives the handler from whoever owns the live entry now (refusing CE's own relay via `InlineHook::IsInTrampolinePool`). This covers the guarded Steam invoke, the preserved-trampoline forward — which re-issues that same frozen jump rather than original code bytes — and the SL fast-path; all fail closed to the clean DXGI bypass. Coverage: `tests/test_dxgi_shared_part13.cpp` (`DXGISharedForeignHandlerValidityTest`).

### Known limitations

- A second overlay that loads *after* CE has already prepended cannot be un-prepended retroactively. That state is named once in the log: `DllNotification: third-party overlay <name> joined a Present entry CE already prepended over ...`. Starting the second overlay before the game takes the wrapper-only path.
- **RESOLVED (0.1.5946):** where an FG interposer previously kept CE at the entry, the Streamline runtime swapchain is now wrapped (non-retaining) and CE leaves the entry ownership-checked, so `DetourPresent: foreign re-hook took the Present entry from CE` no longer occurs in the Talos + DLSS FG + Steam + RTSS configuration. Residual limitation: if a foreign re-hook already took the entry before the runtime swapchain wrap (late wrap / early re-hook), the un-prepend is refused and the old limitation applies.
- **RESOLVED (2026-08-12):** "a swapchain created before injection cannot be wrapped retroactively" no longer means CE is blind — the deep body hook is a wrapper-independent view. It is installed at the install-time leave-entry decision only; the FG un-prepend transition still relies on the wrapped runtime swapchain.
- **A swapchain-wrapping proxy decides where CE's Present hook lands — resolve the SYSTEM dxgi factory (0.1.5962).** Going below the *entry chain* only fixes layering against byte-patching overlays (Steam, RTSS, Detours-style tools), because they all forward the same `this` into the same function body. A proxy that ships as `dxgi.dll` in the game directory and wraps the swapchain *object* (ReShade; SpecialK and OptiScaler have the same shape) is different: `GetPresentAddress` reads slot 8 of the *wrapper's* vtable, so CE's prepend and CE's deep body patch both land in the proxy's own `Present` **prolog** — above its effect pass and above every overlay that patched the real `dxgi!Present` the proxy forwards to. `installed/captureengine/logs/20260812_195840` is the proof: CE logged `[OVERLAY LAYER] CE composites BELOW the foreign Present chain` and Steam still drew on top, with `presentAddr=00007FF95309C140 is in module: …\Talos1\Binaries\Win64\dxgi.dll` and `no visible jump at 00007FF95309C140` (Steam's patch was on the system function further down, not on the proxy's method). Fix: `HookSwapchainVTableViaTempSwapchain` creates its temp swapchain from the **system** DXGI factory (`GetSystemDXGIModuleHandle`, resolved by full path under `GetSystemDirectory` because the proxy shares the base name) via that factory's own `CreateSwapChainForHwnd` slot, and accepts the result only when slot 8 lands inside the system image (`IsAddressInsideSystemDXGI`) — a proxy that also hooks real factory vtables falls back to the historical view unchanged. Log markers: `Created temp swapchain via the SYSTEM dxgi factory (Present=…) — Present hooks target the terminal dxgi!CDXGISwapChain::Present, below the swapchain-wrapping proxy <path>` vs `System-DXGI temp swapchain still resolves Present to … outside the system image`.
  - **VALIDATED** (`installed/captureengine/logs/fixed`, 0.1.5963, user-confirmed): terminal Present resolved into `C:\WINDOWS\system32\dxgi.dll`, `E9 at 00007FFA04FC9960 -> 00007FF9C4FC0000  foreignJumpVisibleNow=1` (Steam/RTSS do patch the system function), deep body hook below it, `[OVERLAY LAYER] … BELOW …`, CE's overlay on top.
  - **The temp swapchain must enter NO foreign handler** (0.1.5964, after two crashes on the first launch of 0.1.5963 in `20260812_201336`): calling the system factory's `CreateSwapChainForHwnd` slot as found entered RTSS and then Steam — `capture_hook!CreateTempSwapChainViaFactorySlot -> RTSSHooks64 -> gameoverlayrenderer64!OverlayHookD3D3 -> 0x0` (DEP execute at null), and in the other launch an `OverlayHookD3D3+0x14bc4` self-recursion until the stack was gone. Steam's overlay dispatches through callback slots that stay NULL until it has rendered on a real game swapchain. The helper now requires the resolved slot to lie inside the system image (otherwise it refuses and the caller falls back) and bypasses a foreign entry patch with `InlineHook::CreateBypassTrampoline` rather than executing it.
  - Consequence to keep in mind: below an object wrapper CE receives the proxy's `_orig` swapchain pointer rather than the wrapper pointer. That is one consistent pointer for every present in the mode (the proxy always forwards the same object), so pointer-keyed state stays coherent, but resize/identity bookkeeping that was populated above the wrapper will not match it.
  - **Still open:** the DX11 install path (`DX11Hook` temp `D3D11CreateDeviceAndSwapChain` -> `DXGIShared::InstallHooks`) has no equivalent terminal resolution. It has not been observed to matter (D3D11's internal factory is not the proxy's), but a `d3d11.dll`-shaped proxy would put CE above it the same way.

### Rejected approaches (do NOT re-pursue)

All four were implemented, shipped, and measured; each excluded a different overlay:

1. **Owner-based chain classification alone** (0.1.5927, `9c023489`) — correct classification, RTSS still vanished (`20260812_001959`).
2. **Frozen install-time relay target** — RTSS drew frame 1 only (`20260812_002958`).
3. **Follow the live entry** (0.1.5931, `5aca4a2e`) — identical outcome (`20260812_005530`, `_010529`); the live entry is Steam's, whose chain has already lost RTSS.
4. **Invoke RTSS's own handler directly** (0.1.5932/0.1.5933, `e3085d89`/`52e93bd2`) — a hardcoded `RTSSHooks64+0x72F20` RVA plus a ±1 MB thunk scan. RTSS drew every frame and **Steam's overlay never drew at all** (`20260812_013241`). Also a build-specific hardcode, which the project rules forbid.

### Historical symptom record

- Symptom (session `installed/captureengine/logs/20260811_233748`, Strange Brigade DX12, build 0.1.5927): with Steam overlay (`gameoverlayrenderer64.dll`) AND RTSS (`RTSSHooks64.dll`) both loaded, RTSS's on-screen display disappeared after a brief moment while CE's overlay stayed. Reproduced with both RTSS inject modes.
- Why the chain was misclassified: the tracked-overlay cache reports the **first loaded entry by list priority**, so with Steam + RTSS loaded it returns `gameoverlayrenderer64.dll` even though RTSS loaded later and displaced Steam's entry jump. CE therefore treated the preserved foreign `E9` at `dxgi!Present` (RTSS's runtime-allocated thunk) as a "Steam trampoline chain" and ran the guarded Steam invoke machinery on it. RTSS's handler is also entered via RTSS's restore/rehook cycle, which re-enters Steam's handler nested inside the call (RTSS saved Steam's `E9` bytes as its "original" prologue).
- Ground truth (dx12_test live probes, session `20260811_235651`): with RTSS only (no Steam), CE's plain trampoline forward keeps RTSS's OSD visible indefinitely (verified by window capture); the same build/CE-overlay combo only fails when Steam is also loaded and wins the name-priority classification. RTSS's thunks are `FF 25 00 00 00 00` + absolute pointer into `RTSSHooks64.dll` (no module backs the thunk itself, so `GetModuleHandleEx(FROM_ADDRESS)` reports "unknown"); Steam's thunks are unresolvable the same way.
- First fix landed (0.1.5927, commit `9c023489`): foreign-chain classification is now owner-based:
  - `ResolveExternalPresentHookThunkTarget` follows the `FF 25` thunk pointer to the real handler; `IsExternalPresentHookSteamChain` then resolves the owner module. Unresolvable thunks fall back to load-order evidence (`IsSteamExternalChainOwnerByLoadOrderEvidence`): the most recently loaded overlay owns the entry jump, so RTSS-as-last-loader means the chain is NOT Steam's.
  - Last-loaded tracking (`LastLoadedTrackedOverlayModuleIndex`) is updated ONLY from real load notifications (`LdrRegisterDllNotification` callback and LoadLibrary hooks), never from the seed walk or the identity-refresh enumeration (which see modules in non-chronological order).
  - `IsSteamExternalChainTrampoline`, `TryInvokeGuardedExternalSteamOverlayPresent`, `ShouldForceSteamDX12Bypass`, the DX12 startup-pass Steam decision, the SL fast-path Steam guard, the fallback `oPresent` decisions, and the Present1 chain classification all use the owner-based predicate.
  - RTSS chains now take the plain preserved-trampoline forward (the dx12_test-proven path). When Steam is ALSO loaded, the forward still runs with Steam's NULL-callback slots patched and the crash-time VEH recovery armed, because RTSS's restore/rehook re-enters Steam's handler inside the call.
- Diagnostics: `InstallPresentInlineHooks` now logs `External hook owner: <module> (thunk resolved)` or `<unresolved thunk>` with the last-loaded/loaded overlay names.
- Regression tests: `tests/test_overlay_compat.cpp` (last-loaded tracking, load-order owner decision), `tests/test_dxgi_shared_part11.cpp` (source-policy: owner-based classification in all Steam routing decisions, install-time owner log, nested-Steam guard in the trampoline branch, notification-only last-loaded recording).
- **Validation result (session `20260812_001959`, build 0.1.5928): NOT FIXED.** The classification fix worked (log shows `External hook owner: <unresolved thunk> (lastLoadedOverlay=RTSSHooks64.dll ...)`, the chain takes the non-Steam trampoline forward, no guarded Steam invoke lines) — **but RTSS's OSD still disappears.** So the Steam guarded-invoke machinery was NOT the cause.
- Current evidence: the game runs ~1200 fps steady; ECL-timing counts start at ~3.0 ECLs/frame (game 1 + RTSS 2, same as the working dx12_test baseline) and drop to ~2.0 then ~0.9 over ~10 s — i.e., RTSS's per-frame submissions stop while the game keeps presenting. In the working dx12_test session (RTSS only, 144 fps) the composition stays 3.0/frame for 6+ minutes. The remaining differences: Steam's overlay is loaded and runs NESTED inside RTSS's handler (RTSS saved Steam's `E9` as its "original" bytes and restores it before calling its "next"), the game uses SyncInterval=0/ALLOW_TEARING at very high FPS, and the scene changes (menu -> gameplay).
- Those open questions are answered: the high frame rate / `SyncInterval=0` present pattern, RTSS's D3D11On12 state, and the menu -> gameplay scene change are all **not** the cause. The finer ECL trace sample (every 64th call, commit `f8f3ecd2`, trace flag `ce_dx12_trace`) is what produced the call trails in the resolved section above.

## RESOLVED: Third-party proxy queue re-entry in the ECL/Signal trace hooks (Talos + ReShade, builds 0.1.5991 / 0.1.5995)

- ReShade's D3D12 command queue is a proxy object: its ExecuteCommandLists/Signal thunks lock a private mutex at `queue+0x58` and forward through `_orig` (the real queue) at `queue+0x10`. CE's per-API "first captured original" globals (`oExecuteCommandLists`, `oTraceCommandQueueSignal`) were taken from the first queue vtable CE hooked — a ReShade proxy — so the layered chain was `game -> CE -> ReShade thunk(proxy) -> CE (real queue) -> global(real queue)`. Calling ReShade's thunk with the REAL queue (a) threw `std::system_error(EDEADLK)` from its non-recursive mutex re-lock in ExecuteCommandLists (session `20260813_041416`) and (b) jumped through a garbage `_orig` vtable slot `-1` in the Signal thunk (session `20260813_050515`).
- The ECL recursion-break resolver (`ResolveECLRecursionBreakTarget` in `hook/apis/dx12_hook_ecl.cpp`, policy `hook/common/dx12_overlay_policy/ecl_recursion_break.h`) now classifies every candidate by owning module and is type-safe by construction: a native D3D12 runtime ECL is only used for a queue whose vtable is native, and a proxy queue is only forwarded through the original taken from its exact vtable. Known foreign overlay hooks and CE's own detour are never re-entered; a recursion-depth bound drops the submission instead of looping.
- Native originals are published eagerly as `dx12_hook_g_RealD3D12ECL` / `dx12_hook_g_RealD3D12Signal` whenever a queue vtable hook still exposes a d3d12/d3d12core slot (`TryPublishRealD3D12*Candidate` called from `DX12_HookQueueVTable`).
- The Signal trace forward now resolves per-vtable originals (`dx12_hook_g_CommandQueueSignalOriginalByVTable`) and falls back to the queue's live vtable slot, the resolved native Signal, then the legacy global — never the blind global first (`DetourTraceCommandQueueSignal`, `hook/apis/dx12_hook_ecl_install.cpp`).
- Regression coverage: `tests/test_dx12_ecl_recursion_break_policy.cpp` (classification/selection policy plus source pins for the ECL break path, the per-vtable Signal forward, and eager native publication).

## RESOLVED: x86 DX12 overlay DEVICE_HUNG (dx12_test) — fixed 2026-06-09
Single hand-off reference: `handoff-dx12-32bit-crash.md`. Chronology: `log/recent.md` (2026-06-08..09).

### Current Fix
- The 32-bit DX12 test crash was isolated to overlay text draws that sampled CE-owned font resources. Full DRED showed the solid draw completing and the first textured/font-resource draw hanging; every resource-sampling text path failed, while the all-solid diagnostic passed.
- Corrected uncapped rendering reproduced the hang without a focus change. Alt+Tab/independent-flip transitions amplified the failure but were not required, so focus state is not the final root-cause boundary.
- NVIDIA x86/WoW64 driver behavior is the leading explanation, not a proven vendor root cause: there is no standalone non-injected reproducer, vendor confirmation, or cross-vendor/driver matrix in the retained repository evidence.
- The fix keeps native direct DX12 overlay rendering. It does not use pseudo overlay, DirectPresent overlay, D3D11On12, sleeps, or a focus-transition offscreen/copy fallback.
- x86 DX12 now routes text through solid glyph spans: `FontAtlas` builds alpha spans, `RendererBackend::PreferSolidTextGeometry()` requests solid text, and `DX12Backend` enables it for x86 via `ShouldUseSolidDx12TextGeometryForProcess`.
- `DX12Backend` skips font SRV upload when a frame has no textured commands. Healthy x86 no-FG logs show one solid command (`textured=0`) and `DX12 Overlay: x86 solid-span text path enabled`.
- The v13 marker is `DX12 focus-loss sync policy=v13 draw-every-frame + x86 solid-span text + upload-slot per-frame fence`.

### Validation
- Build `0.1.3822`: `python build.py --skip-updates` succeeded.
- Focused tests passed for glyph spans, solid-text renderer commands, x86 DX12 backend/text policy, upload/focus-loss policies, and binary log markers.
- Runtime: six total 30 s runs of `installed/testapp/x86/dx12_test.exe` with x86 `testappconfig.ini` (`fullscreen=1`, 4K, `gpu_load=120`, `vsync=0`), overlay enabled, `observer_only=false`, DRED disabled for low perturbation. All stayed alive at 30 s, had zero not-responding samples, and no device removal.
- Fresh-session log dirs: `installed/captureengine/logs/20260609_000749`, `20260609_000823`, `20260609_000858`.

### Historical Symptom (Superseded)
Injected **32-bit** `dx12_test.exe` in borderless-fullscreen (4K, vsync=1) freezes ~2–3.8 s on Alt+Tab in/out → GPU `DEVICE_HUNG (0x887A0006)`, a **real GPU TDR** (`DxgKrnl/TdrCaptureDumpStart/Finish` in the GPUView trace). **64-bit never freezes.** **Bare 32-bit (no CE) never freezes. app+RTSS never freezes.** So CE is the trigger.

### Historical Alt+Tab stall manifestation (confirmed observation, not final root cause)
A mid-stall watchdog dump showed the **application's own `ExecuteCommandLists`** blocked inside a **kernel GPU virtual-address map**: `dx12_test!Render → capture_hook!DetourExecuteCommandLists → D3D12Core!CCommandQueue::ExecuteCommandLists → nvwgf2um (NV UMD) → NDXGI::CDevice::MapGpuVirtualAddressCB → win32u!NtGdiDdDDIMapGpuVirtualAddress` (blocked in VidMm). The 2 s GPU TDR then fired. The dump was taken during the stall (`logs/20260606_211023`); `logs/20260608_162931` recorded a crash variant in the same NV UMD path. This confirms how one Alt+Tab failure manifested on that x86/NVIDIA system, but later steady-state DRED narrowed the actionable trigger to CE font-resource text draws and showed that focus change was not required.

### Historical Alt+Tab trigger boundary: CE native backbuffer activity
`observer_only=true` (CE injected, hooks active, but no overlay GPU resources or submissions) did not freeze under extreme Alt+Tab (`logs/20260607_003611`, `logs/20260608_163139`). This established that CE GPU work, rather than mere hook/device presence, was necessary for that transition-time manifestation. Historical v8/v9 DRED implicated both direct draw and a backbuffer copy, but that experiment did not identify the final draw-shape boundary; the later uncapped isolation did so by comparing resource-reading text with resource-free solid text.

### ELIMINATED with evidence (do NOT re-pursue)
- **GPU residency / eviction** — DISPROVEN. The in-process focus-analysis flight recorder (`[Overlay] dx12_focus_analysis=true`, `IDXGIAdapter3::QueryVideoMemoryInfo`) shows local Budget=11175 MB / Usage=81 MB **rock-flat through the 3.8 s stall** (usage 0.7 % of budget, never over-budget); CE's overlay adds only ~6 MB vs observer-only (75 MB). (`logs/20260608_162931` vs `163139`, `170854`.)
- **Workload magnitude** — DISPROVEN. RTSS does MORE ECLs (52 vs app 38) + MORE fence Signals (64 vs 26) + a far larger footprint (~24 CUSTOM-heap resources + two 1,000,000-descriptor heaps) and never freezes.
- **"iflip disabled by the debug layer"** — DISPROVEN. Enabling the D3D12 debug layer (`ce_dx12_debug_layer`=1) PREVENTED the historical Alt+Tab manifestation (13 edges, no stall, `logs/20260608_171158`) while the trace still showed `MMIOFlipMultiPlaneOverlay`. The layer therefore perturbed timing rather than disabling iflip; this did not establish the final underlying driver mechanism and was never a shippable fix.
- **Per-frame submission count / DMA-pool, loader stalls, forced-on DRED** — earlier real contributors, all fixed/excluded; freeze persisted.

### What RTSS actually does (observed empirically via the CE call-trace — see Tools)
RTSS renders its overlay via **D3D11On12** (`trail: ...>d3d11on12.dll>d3d11.dll>RTSSHooks.dll`), submitting on the **app's** queue, and survives the transition because the **D3D11 runtime owns the wrapped-resource Acquire/Release/Flush**. This is exactly the technique forbidden by AGENTS.md ("use native DX12"). So the open problem is: make raw native D3D12 backbuffer-touch survive the 32-bit iflip transition the way the D3D11 runtime does.

### Historical Fix-Space (Superseded By v13)
D3D11On12, DComp/composited separate-surface overlay, hiding the overlay during the transition, pure timing/sleep bandaids, and dedicated non-FG backbuffer queues were all rejected or invalid. The accepted v13 fix keeps native direct DX12 overlay rendering and removes x86 DX12 font-resource text sampling by drawing text as solid glyph-span geometry.

### Diagnostic tools (committed, gated, OFF by default)
- **`ce_dx12_dred` flag file (empty = page-fault-only, low perturbation; `1`/`full` = auto-breadcrumbs) or env `CE_DX12_DRED=pf|1`** → DRED on device-removed: `DX12 DRED: pageFaultVA=.. [existing]/[recently-freed] ..` (+ breadcrumb op in full mode). **Page-fault-only is the right tool for the steady-state DEVICE_HUNG** (full auto-breadcrumbs perturb timing and can mask it). Code: `ce::dx12_dred` (`hook/common/dx12_dred.cpp`), `DredArmMode`/`DecideDredArmMode` (`hook/common/dx12_overlay_policy.h`).
- `[Overlay] dx12_focus_analysis=true` (config) → in-process residency flight recorder + present-gap + CPU VA-space probe (`vaspace committedMB/freeMB/largestFreeBlockMB`, ~1/s and at the stall). **RESULT: VA is FLAT through the stall — the 32-bit VA/command-buffer-pool exhaustion hypothesis is RULED OUT.** Still useful as the residency/present-gap flight recorder. Code: `Dx12SampleVaSpace`/`DX12_UpdateFocusAnalysis`/`DX12_DumpFocusAnalysisRing` in `dx12_hook_focus_loss.cpp`.
- `ce_dx12_trace` flag file (or env `CE_DX12_TRACE=1`) + `tools/tracing/dx12_call_trace.py` → caller-attributed D3D12 call trace (CreateCommandQueue/Resource/DescriptorHeap, ExecuteCommandLists, Signal, CreateSwapChain). Logs: `DX12 TRACE:`. NOTE: CE's own overlay ECL/Signal use the raw `realECL` pointer so they are NOT captured (a known blind spot); it captures the app's and co-resident modules' calls.
- `tools/tracing/gpu_trace.py capture [--debug-layer N] [--open]` → automated GPUView kernel capture (wraps in-box `gpuview/log.cmd`; needs an ELEVATED shell; user triggers the Alt+Tab; auto-stops on the dump, merges to `Merged.etl`, coarse-parses).
- `ce_dx12_debug_layer` file (`1`=layer, `2`=+GPU validation) → D3D12 debug layer (`DX12 DBGLAYER:` lines). NOTE: enabling it MASKS the freeze (timing).

### Key repro log dirs
`20260606_211023` (mid-stall dump = ground truth), `20260608_162931` (focus-analysis: flat residency + UMD AV crash), `20260608_163139` (observer-only: no freeze), `20260608_170854` (clean GPUView: TDR confirmed), `20260608_171158` (debug-layer: no freeze, iflip still on).

## Facts
- The tree currently identifies known third-party overlays primarily by module-path tokens such as `gameoverlayrenderer`, `discord_hook`, `socialclub`, `eosovh`, `eossdk_win64_shipping`, `nvspcap`, `nvoverlay`, `rtsshooks`, and `specialk`.
- A smaller startup-blocking subset is tracked separately. Current tokens include `socialclub`, `eosovh`, and `eossdk` variants.
- If a third-party overlay is already loaded before the real D3D12 device exists, the DX12 policy can defer early temporary-swapchain `Present` hook installation to avoid recursion and stack-overflow startup failures.
- Startup overlay compatibility is driven by observed overlay and runtime state, not by process name.
- During startup compatibility mode, overlay rendering is only considered safe once a live swapchain queue is known and the swapchain is no longer runtime-owned, or the late pre-FG runtime-owned handoff has remained stable long enough to treat that queue topology as settled.
- A few successful startup overlay draws on the original game queue are not by themselves proof that startup compatibility is over. If a startup-blocking overlay is still loaded and a later pre-FG runtime-owned swapchain handoff appears before any real FG activation has been observed, startup compatibility must re-arm and keep the conservative suppression path active through that handoff.
- Once that late pre-FG runtime-owned handoff has settled enough to render again, startup compatibility must still stay active until real FG is observed or runtime ownership returns to the normal non-runtime path. Otherwise a single successful draw on the handoff queue immediately drops CE back onto the normal coexistence path on the next frame even though startup bootstrap is still in progress.
- That late pre-FG runtime-owned handoff re-arm must be keyed off the handoff edge itself, not the steady-state `runtimeOwnsSwapchain` flag. Otherwise CE can re-arm startup compatibility every frame for as long as the runtime-owned swapchain remains present, repeatedly resetting staged startup activation even after the topology has already settled enough to render safely.
- Third-party overlay swapchains and private queues are not allowed to become authoritative game state just because they call into our hooks.
- If an immediate caller looks like a third-party overlay but FFX FG stack or module evidence is present, the FFX evidence can override the misleading caller identity.
- Dynamic `GetProcAddress` caller filtering has a narrow FFX exception: generic D3D/DXGI hooks are still hidden from third-party overlay callers, but `ffxCreateContext`, `ffxDestroyContext`, and `ffxConfigure` stay visible when the target module is an official FFX runtime. GTA/EOS can route native FSR startup through an overlay-looking caller, and hiding those FFX APIs prevents CE from installing the real present-callback bridge before overlay GPU work resumes.
- FFX dynamic export hooks must be registered before process-wide `GetProcAddress` interception is enabled. Otherwise an early FSR preload can call `GetProcAddress(ffxConfigure)` while the router is active but before the FFX names are registered, cache AMD's original function pointer, and later run native FSR without CE's callback bridge. `hook/main.cpp` calls `FFXHook::RegisterDynamicHooks()` before `IATHook::InitializeGetProcAddressHook()` so official AMD modules can use the IAT/dynamic route from the first preload.
- IAT/dynamic FFX routing is not sufficient for every official SDK integration. `installed/captureengine/logs/20260530_234519` showed the switch app entering protected official FFX startup and then staying quiesced while app-side FSR callbacks were firing because CE never saw `ffxConfigure`. Official AMD DX12 modules therefore also arm a guarded re-arming `ffxConfigure` VEH fallback that catches SDK dispatch-table or intra-module calls while standard inline JMP hooks remain disabled. Healthy logs include either `GetProcAddress: Intercepted FFX API ffxConfigure` or `FFX Hook: Armed VEH breakpoint for ...!ffxConfigure`, followed by `Direct FFX API confirmation established from ffxConfigure ENABLED`.
- Dynamic `GetProcAddress` filtering also has a narrow Streamline proxy exception: `CreateDXGIFactory*` exports from `sl.interposer.dll` must remain the real Streamline proxy exports. Hiding them behind CE wrappers makes the application create a CE/raw DXGI factory, prevents Streamline from owning its swapchain interposer, and can later crash the DLSS-G handoff path. This exception is only for Streamline's proxy DXGI factory exports; CE still hooks Streamline feature APIs such as `slDLSSGSetOptions` / `slDLSSGGetState` through the feature-hook paths.
- **Synchronous foreign-Present calls require source-thread provenance whenever a runtime can Present from workers.** Talos session `installed/captureengine/logs/20260809_015416` supplied two dumps five seconds apart with the same `sl.dlssg` worker blocked in `WaitForSingleObjectEx -> gameoverlayrenderer64 -> capture_hook_x64!TryInvokeGuardedExternalSteamOverlayPresent`. CE had called Steam with reason `SL startup bypass`; game/render/RHI progress then waited downstream for 51 seconds. The Streamline plugin-lookup and Steam NULL-callback VEH guards prevent two crash/re-entrancy families, but cannot make an unbounded third-party handler thread-safe or nonblocking. `TryInvokeGuardedExternalSteamOverlayPresent` checks every runtime-owned presentation signal and permits Steam only when the current thread equals the previously proven `DX12_GetGamePresentThreadId`; unknown or worker provenance fails closed to the existing DXGI bypass. The tracker itself refreshes only from calls already classified as application-source Presents, so neither Streamline nor FFX workers can promote themselves by overwriting provenance. Never replace this with a timeout, cancellation, or dispatch to another worker. **2026-08-09 refinement (build 0.1.5900):** `applicationSourcePresent` must be derived from `ce::dx12_overlay_policy::IsRuntimeGeneratedFrame` (no-callback FSR, Streamline FG active, runtime-owned swapchain, FFX caller, runtime-owned native FSR), NOT from the broad `frameGenerationPresentationActive` set that includes `callerFromStreamlineModule`. The interposer forwards the game's own real-frame presents even while FG is off; classifying them as runtime-generated permanently prevents the tracker from ever latching, leaving `sourceTid=0` and making the Steam overlay invisible for the whole session (observed in every retained DLSS session before 0.1.5900; `tests/test_dxgi_shared_part10.cpp` pins the classification and the wiring).
- **The natural Steam E9 transport in `CallOriginalPresent`'s SL fast-path got the same protections on 2026-08-09 (RoboCop crash).** RoboCop: Rogue City session `installed/captureengine/logs/20260809_140551` crashed the RHI thread with RIP=0: once DLSS FG turned on, `CallOriginalPresent`'s SL fast-path (`slLoaded && presentOriginal && presentOriginal != DetourPresent`) called `dxgi!Present` through Steam's E9 JMP, and `gameoverlayrenderer64!OverlayHookD3D3` called a NULL internal rendering callback (the temp-swapchain pre-init initializes Steam's "next" handler but not the rendering callback on the real swapchain; the gameoverlayrenderer64 build was 2026-08-03). Until then this path had neither the source-thread provenance rule nor the NULL-callback VEH recovery that every other Steam transport carries. It now (a) fails closed to the bypass trampoline when a worker-capable FG runtime presents from a non-source thread, and (b) runs under `ScopedSteamNullCallbackRecoveryGuard`, so a NULL callback is patched to CE's DXGI bypass and retried instead of crashing. Source-thread presents with a valid Steam callback (Talos) are unchanged. Source anchors: `hook/common/dxgi_shared_original.cpp` (SL fast-path), `tests/test_dxgi_shared_part11.cpp` (`SlFastPathSteamTransportIsGuardedLikeEveryOtherSteamTransport`).
- **Streamline runtime recognition is name-independent (2026-08-09).** NVIDIA Streamline can load its runtime DLLs under obfuscated hashed names (`1B0_E658703.dll` in RoboCop, confirmed in the crash dumps), which contain none of the `sl.*` path tokens. `IsStreamlineModuleHandle` (`hook/common/dxgi_shared_steam.cpp`) therefore also matches modules that export the Streamline plugin API (`slGetPluginFunction` / `slGetFeatureFunction`), with a small per-module cache because the check runs on the Present classification path. Without this, `callerFromStreamlineModule` stays false for every runtime-originated Present, PostSL routing cannot classify them, and the overlay starves after the first confirmed frame (RoboCop session `20260809_144640`). The late-handoff PostSL activation fallback additionally marks `streamlineStartupTopLevelPresentConsumed` when it retains the live swapchain, so the 8-frame confirmed-startup settling window is covered by the keep-startup route instead of deadlocking (the create-time arming never ran because origGame was unknown at swapchain create).
- **CE must never write into Steam's Present-shaped callback slots (2026-08-12; REVERSES the 2026-08-09 proactive patch).** The 2026-08-09 `EnsureSteamNullCallbacksPatched` scanned `gameoverlayrenderer64.dll` for `mov (e)ax,[slot] ... call (e)ax` sites and pre-filled every still-NULL slot (20 of them in both Strange Brigade and Talos) with CE's DXGI bypass, to stop a NULL dispatch from faulting. Those slots are **Steam's own hook-install outputs**: `gameoverlayrenderer64+0x8da00` receives `&slot`, and each call site tests `cmpq $0, slot` immediately afterwards, so Steam initializes them lazily and treats non-NULL as "already installed". Pre-filling therefore makes Steam skip its own install **and** turns its "call original" into a raw `dxgi!Present` copy that skips every hook chained BELOW Steam. Talos + DLSS FG + RTSS (`installed/captureengine/logs/20260812_022607`): frame 1 ran `sl.dlss_g -> CE -> Steam -> RTSS` with all three overlays drawing; from frame 2 every guarded invoke reported `steamCallback=<CE's own bypass>` and RTSS submitted nothing for the rest of the session (RTSS 1 ECL, Steam 77, game 200). The write is removed. The NULL dispatch is handled by the two sound mechanisms that were already present: `ShouldInvokeGuardedExternalSteamOverlayPresentForCallbackState` refuses the invoke unless the recovery is armed, and `SteamOverlayInitVehHandler` resolves the **exact** faulting slot from the fault context (`ResolveSteamNullCallbackSlotFromFault`) and recovers only that one. Slot discovery stays, read-only, to feed the gate. Coverage: `tests/test_dxgi_shared_part12.cpp` (`CENeverWritesIntoSteamCallbackSlots`). **Invariant: CE may inspect another tool's internal state, never speculatively write it.**
- On Steam's E9/vtable topology, `DetectSLPresentHook()` intentionally cannot establish physical SL routing and `s_slRoutingActive` remains false after PostSL is stable. The `callerFromStreamlineModule && !s_slRoutingActive && steamOverlayLoaded` block is consequently a lifetime external-overlay **transport guard**, not evidence of startup state. Source Presents may service Steam there; generated worker Presents must bypass Steam while retaining CE's established PostSL draw.
- If the effective runtime mode is FSR FG, SL routing must stay suppressed even if the SL hook remains physically present on `Present`/`Present1`. Re-enabling SL routing in that state can deadlock the render thread inside the FFX runtime.
- The forced-bypass Steam rule is split by FG owner and thread provenance. When Streamline is merely loaded and Streamline FG is not running, bypass-only remains the safe no-FG/startup default. When native FSR owns presentation, CE may try the guarded Steam Present hook only on the verified source Present thread; a runtime worker or unknown thread uses the DXGI bypass. Talos `installed/captureengine/logs/20260531_230835_talosfsrfg` exposed the earlier missing FSR exception, while `20260809_015416` proved that invoking Steam indiscriminately from an FG worker deadlocks presentation.
- Current DXGI startup pass-through windows are short and explicit: normally 3 frames, or 16 frames for Steam when bypass is available.
- **The dedicated DX12 overlay queue is FG-ONLY (`ShouldUseDedicatedDX12OverlayQueue` returns `actualFGActive`).** A 2026-06-06 attempt to enable it for plain non-FG (to keep CE's overlay `ExecuteCommandLists` off the app's shared command-queue pool) was **REVERTED**: DXGI forbids a non-owning queue from rendering the swapchain backbuffer and returns `DXGI_ERROR_ACCESS_DENIED (0x887A002B)`, removing the device on the FIRST overlay submit (proven at 64-bit startup `logs/20260606_153428`: "DEVICE REMOVED after reinit submit #1", queue=dedicated, offscreen=0 → black window). **Only the swapchain's owning (app) queue may render the backbuffer.** A dedicated queue could therefore only do OFFSCREEN overlay work with the composite-onto-backbuffer still on the app's queue — it cannot remove CE's backbuffer submission from the app's queue. So "dedicated queue to offload the app's queue" is a dead end; don't retry direct-draw on a non-owning queue.
- A post-FSR `FSR_FG -> DLSS_FG` comeback can hit a distinct third-party coexistence seam from the older unsafe-bootstrap failures: CE may already have enough shared-state evidence to keep the startup family on the normal Streamline route and invoke PostSL there, while Steam's DX12 hook for the fresh swapchain still has a stale or null saved original Present pointer. In that state, falling through `oPresent` re-enters `gameoverlayrenderer64` and Steam can crash even after the first recovered PostSL render if the path is still inside the short confirmed-startup-settling window.
- The current generic rule is therefore split by concern. The startup-routing decision stays topology-driven (`keepStartupPresentOnNormalRoute`), but the actual Present transport on DX12 post-FSR comebacks uses the bypass trampoline until PostSL has both confirmed a successful render and left the short confirmed-startup-settling window. This keeps third-party overlay coexistence generic: CE does not trust Steam's fresh-swapchain vtable hook state just because the higher-level post-FSR startup family is already safe enough to continue normal-route PostSL progress.
- Transition cooldowns are routing hints, not hard reasons to blank an already drawable DX12 overlay. `ShouldHeavySuspendDX12OverlayForSwapchainState(...)` only hard-suspends for zero-sized swapchains or iconic windows; ordinary swapchain/focus/FG-transition cooldowns should keep the initialized overlay visible if the backend remains valid. The expected degraded behavior during a fragile transition is a very brief visual stall, not overlay disappearance.
- Post-FSR DLSS comeback swapchain churn is also not a reason to blank a proven PostSL overlay route. GTA `installed/captureengine/logs/20260531_232108` showed CE had already rendered through PostSL on Streamline's runtime-owned queue, then an ordinary swapchain-change cleanup destroyed that backend during the 30-frame confirmed warmup proof window and GTA raised `ERR_GFX_STATE`. The current rule preserves that confirmed backend only for the narrow active FSR-to-DLSS runtime-owned queue handoff and clears stale cached swapchain pointers without resetting PostSL state.
- Startup-overlay compatibility windows from Social/EOS/Steam-like modules should not blank an already initialized DX12 overlay. `ShouldDelayDX12OverlayRenderAfterSyncInit(...)`, `ShouldSuppressDX12OverlayRenderForLoadedStartupOverlay(...)`, and `ShouldSuppressDX12OverlayRenderForRecentBlockingRendererActivity(...)` accept backend-ready state, and `ShouldKeepDX12OverlayVisibleDuringStartupSuppression(...)` documents the invariant: once CE has a live DX12 overlay backend and sync state, compatibility suppression should stop new-risky initialization work but continue overlay submissions.
- If Social/EOS/Rockstar-style startup layers create a Streamline-adjacent runtime-owned swapchain after CE has already rendered stably and no real FG activation has appeared, that is a runtime-inactive handoff, not proof that DLSS-G started. In that settled no-FG case, CE preserves the live DX12 overlay backend, clears only stale cached swapchain pointers, avoids startup compatibility re-arm/deferral, skips startup resource priming/post-prime settle delays that would blank the already-visible overlay, and keeps the overlay on the original game queue until a real FG signal arrives. Healthy logs include `Keeping settled startup overlay live through runtime-inactive Streamline handoff`, `Fresh authoritative Streamline no-FG handoff preserved live overlay backend`, `Skipping startup resource priming delay because live overlay is preserved through runtime-inactive Streamline handoff`, and `Overlay kept visible during runtime-inactive Streamline startup handoff`.
- **x86 DX12 v13 text path supersedes the old focus-transition hold/offscreen history.** Focus loss must not hide the overlay. Current x86 DX12 no-FG rendering stays on the native direct path and emits overlay text as solid glyph-span geometry, so healthy traces show `DX12 focus-loss sync policy=v13 draw-every-frame + x86 solid-span text + upload-slot per-frame fence` and `DX12 DIAG: Texture2D command ... textured=0`. Older v10 hold/offscreen guidance below is historical only.
- **Third-party-overlay detection must be loader-free on the Present hot path (an independent x86 hot-path hazard, not the final font-resource hang cause).** `GetLoadedThirdPartyOverlayModuleName()` is called from `DetourPresent` every frame and must NEVER call into the Windows loader. The earlier design walked `GetModuleHandleA` over the overlay list and invalidated its cache on *every* DLL load; Alt+Tab-related DLL activity could therefore force repeated x86/WoW64 loader work on the Present path. Removing that work was valid hardening, but the later uncapped hang reproduced after it was fixed and was ultimately avoided by v13 solid-span text. Current rule: the Present path does a single atomic read; the loaded-overlay set is maintained off the Present thread via a one-time seed scan (`SeedThirdPartyOverlayModuleCacheFromLoader`, HookThread), `LdrRegisterDllNotification` (all load/unload, loader-safe base-name compare + atomic store), and the existing `LoadLibrary`/`LdrLoadDll` -> `NotifyHookModuleLoaded` path — all reacting only to known overlay modules. Canonical-list order is the detection priority (Steam wins over RTSS/Discord/etc.). This is detection-only: it never changes or hides overlay rendering. Healthy logs: `Third-party overlay detection: seed scan ...`, `LdrRegisterDllNotification active ...`, `DllNotification: third-party overlay module loaded/unloaded: ...`.
- **Focus-transition/occlusion safety must be fed on BOTH present paths, but it is not the final x86 text-hang fix.** All non-presentable state (`g_SwapchainPresentOccluded`, the invisible-safe backbuffer-work hold, focus-edge telemetry, and DRED dump-window widening) lives in `DX12_NoteWrappedD3D12PresentResult`. `DetourPresent` feeds that result as well as the wrapper path so a genuinely occluded, iconic, or zero-sized vtable-hooked swapchain gets the same treatment. Earlier investigation over-attributed the test-app failure to missing occlusion wiring; later steady-state reproduction showed the resource-reading text hang could occur while focus transitions were absent. Invariant: presentability signals must cover both hook paths, while mere focus loss must never hide a still-presentable overlay.
- Steam's DX12 overlay hook can be loaded even when the user-visible Steam overlay is disabled. Current Steam builds can expose more than one lazy Present-shaped callback slot inside `gameoverlayrenderer64.dll`; `installed/captureengine/logs/20260531_141812_strangebrigadedx12crash` faulted at `steam+0x162200`, while older notes focused on `steam+0x1621d8`. CE's Steam null-call VEH recovery must resolve the exact slot from the faulting `mov rax,[rip+disp]` / `call rax` instruction and patch NULL slots to CE's DXGI bypass Present when a bypass exists. `SteamDummyRenderingCallback` is fallback-only when no bypass exists. Guarded Steam invocation still skips invalid low-address sentinels and CE dummy slots, but a bypass-patched slot is a real next-Present path and should allow Steam overlay rendering.

## Working Guidance For DX12 Games With External Overlays Active
- Identify startup coexistence problems from module path, queue ownership, swapchain ownership, and call-stack evidence, not from game-specific branches.
- Treat foreign swapchains and queues as non-authoritative until the real game queue or swapchain is proven.
- Use narrow startup bypass windows, then converge back to normal routing as soon as the live game path is clear.
- When FFX stack evidence and third-party overlay identity disagree, do not blindly trust the immediate caller alone.
- Preserve the FFX API dynamic-hook exception when tightening third-party overlay bypass rules. Losing it can make native FSR appear to work while keeping the injected overlay permanently suppressed or falling back through unsafe recovery paths.
- Do not solve coexistence by blanking an already initialized CE overlay. Prefer a route that preserves visibility and only avoids unsafe new initialization or unsafe separate GPU work. Truly non-drawable swapchains such as minimized/iconic or zero-sized surfaces are the narrow hard-stop case.
- Treat late runtime-owned Streamline/no-FG swapchain handoffs from startup overlay stacks as preserve-live-overlay candidates only after the overlay already rendered stably and before any explicit FG activation. Once actual FG appears, use the normal DLSS/PostSL or FFX callback rules.
- During Alt+Tab/focus changes, preserve the DX12 overlay backend and KEEP RENDERING the overlay whenever the window is visible (Present returns `S_OK`). Only hold CE's swapchain backbuffer overlay/capture work when the swapchain is genuinely not presentable (occluded/iconic/zero-size). Healthy traces show `DX12 focus-loss sync policy=v8 visibility-gated-backbuffer-hold`, `DX12: Swapchain presentability changed -> ...`, and (only while not presentable) `DX12: Holding overlay/capture backbuffer work while swapchain is NOT presentable` / `DX12: Resuming overlay/capture backbuffer work`. The old v7 `Holding focus-loss ... while process is backgrounded` / `... during foreground reacquire proof` / `Focus-loss foreground reacquire Present proof accepted` lines are stale and indicate a focus-based hide regression. Frame-latency lines remain skip/pass-through telemetry, not a correctness gate.
- Keep fixes generic across Steam, Rockstar, Epic, and similar overlay stacks. The code already leans toward topology and state-driven behavior; preserve that direction.

- **Vtable hook path critical difference from inline hook path**: When external E9 JMP is detected at `dxgi!Present` (inline hook), CE uses vtable hooking instead of inline hooking. In the vtable hook path, `oPresentTrampoline` is NULL (no inline hook trampoline created). `DetectSLPresentHook()` correctly bails early in the vtable path because `oPresent` (saved vtable[8]) is Steam's hook function, not dxgi!Present — checking Steam's function bytes for an E9 JMP would never detect SL's hook. SL routing (`s_slRoutingActive`) stays false in the vtable path by design, and Steam overlay is invoked through `CallOriginalPresent`'s explicit `g_externalOverlayPresentHook` logic.

- **Startup compat pass crash with vtable path + Steam overlay (build 0.1.2901 fix — no Streamline)**: When the vtable path is chosen (inline hooks skipped due to Steam's E9 JMP on `dxgi!Present`), `oPresentTrampoline` is null. The startup compatibility pass (`kPassThroughOriginal`) calls `CallOriginalPresent`, which falls through to `presentOriginal` (= `dxgi!Present` with Steam's E9 JMP). Steam's `OverlayHookD3D3` runs and tries to call its saved "next" handler via `vtable[8]` → gets `DetourPresent` (CE's detour) → resolution fails → NULL → RIP=0. This happens even without Streamline loaded. Fix: when Steam overlay is active AND `oPresentTrampoline==NULL`, the startup compat pass uses the bypass trampoline (`oPresentBypass`) directly instead of `CallOriginalPresent`. The bypass trampoline contains original `dxgi!Present` disk bytes (no E9 JMP), calling real DXGI Present directly. Source: `dxgi_shared.cpp:1636`.

- **Steam overlay invisible when SL loaded but FG not running (build 0.1.2863 - PARTIAL fix)**: When SL (Streamline) is loaded (`sl.interposer.dll` present) but Streamline FG is not running, `ShouldForceSteamDX12Bypass` returns true in `CallOriginalPresent` / `CallOriginalPresent1`. This causes the path to go directly to the disk-bytes bypass trampoline, which skips ALL inline E9 JMP hooks including Steam's overlay. Initial fix: invoked Steam overlay in `CallOriginalPresent` and `CallOriginalPresent1` before the bypass trampoline.

- **Steam overlay invisible when SL loaded (build 0.1.2866 historical fix; thread-safety assumption superseded 2026-08-09)**: The 0.1.2863 fix was insufficient because ALL Present calls were intercepted by EARLIER return paths in `DetourPresent`:
  - **Startup bypass (DllMain guard, line 1669)**: When `callerFromStreamlineModule=true && !s_slRoutingActive && steamOverlayLoaded`, the code returned early via the disk-bytes bypass trampoline. Never reached `CallOriginalPresent`.
  - **Synthetic re-entrant path (line 1283)**: After DLSS FG activation, Present calls from SL modules go through the synthetic re-entrant path. The `steamOverlaySafe` guard (line 1318) was too restrictive: it required `postSLConfirmedRendering=true`, which never happened during the PostSL warm-up phase.
  - **Confirmed standalone normal route (line 1227)**: Same restrictive `steamOverlaySafeConfirmed` guard.
  
  Proper fix (3 locations):
  1. **Startup bypass** (line 1669): Added Steam overlay invocation before the bypass trampoline, guarded by `!postSLConfirmedButStartupSettling` (prevents DllMain phase crashes). Steam's overlay hook presents the frame through Steam's own trampoline, so the bypass is not needed.
  2. **Synthetic re-entrant** (line 1318): Relaxed `steamOverlaySafe` from `!callerFromStreamlineModule || (postSLConfirmedRendering && !postSLConfirmedButStartupSettling)` to `!callerFromStreamlineModule || !postSLConfirmedButStartupSettling`. The warm-up phase (pre-confirmed rendering) is well past DllMain — SL modules are fully loaded and Steam TLS is initialized.
  3. **Confirmed standalone normal route** (line 1241): Same relaxation for `steamOverlaySafeConfirmed`.
  
  Historical safety assumption, now disproven: `postSLConfirmedButStartupSettling` was treated as the only guard after DllMain. Talos `20260809_015416` proved that a later DLSS-G worker can still block indefinitely inside Steam even with plugin lookup and NULL-callback recovery ready. Current code additionally requires verified source-thread provenance.
  - Primary source anchors: `dxgi_shared.cpp` ~line 1669 (startup bypass), ~line 1318 (synthetic re-entrant), ~line 1241 (confirmed standalone normal route)
  - Root cause: callFromStreamlineModule remains true for ALL Present calls when SL interposer wraps the game's Present calls, causing DetourPresent to take early bypass paths that skip Steam overlay without our explicit invoke.

## Non-SL Steam Overlay Bypass (Strange Brigade DX12 Fix)

### Build 0.1.3612/0.1.3613 — Dynamic Steam null callback slot recovery

- **Inputs**:
  - Strange Brigade DX12 `installed/captureengine/logs/20260531_141812_strangebrigadedx12crash` crashed on the second Steam E9 path after the one-time init had already patched the older `steam+0x1621d8` slot. cdb disassembly showed `OverlayHookD3D3+0x13e3f` loading a NULL function pointer from `steam+0x162200` and calling it with the `Present(swapchain, sync, flags)` signature.
  - Talos `installed/captureengine/logs/20260531_141924_talossteamoverlaydoesnotwork` did not crash, but the Steam overlay never appeared. The log showed the first guarded Steam call patching the legacy slot to CE's dummy callback, then every later frame skipped Steam because the slot was "not a real renderer".
- **Root cause**: Treating the older hardcoded Steam slot as the only relevant callback was stale. Also, patching a Present-shaped Steam slot to a no-op avoids a NULL crash but prevents Steam from chaining to the real Present path, so later policy sees only CE's dummy and bypasses Steam forever.
- **Fix**: `SteamOverlayInitVehHandler` now resolves the exact faulting Steam global slot from the call-site bytes, patches NULL slots to CE's DXGI bypass Present when available, and retries the call. The non-Streamline steady Steam E9 path is now protected by the same scoped recovery guard, not just the first init call. The no-op dummy remains only as a last fallback when no bypass trampoline exists.
- **Validation**: `python build.py --skip-updates` passed with build `0.1.3612`; `python build.py --no-build --run-tests --skip-updates` passed 830 tests with metadata `0.1.3613`. Fresh manual Strange Brigade and Talos Steam-overlay validation is still needed.
- **Source anchors**: `hook/common/dxgi_shared.cpp`, `hook/common/dxgi_shared.h`, `tests/test_dxgi_shared.cpp`, `installed/captureengine/logs/20260531_141812_strangebrigadedx12crash`, `installed/captureengine/logs/20260531_141924_talossteamoverlaydoesnotwork`.

### Build 0.1.2904 — Force bypass for non-SL Steam overlay
- When Steam overlay is loaded without Streamline or NvPresent (e.g. Strange Brigade DX12), `ShouldForceSteamDX12BypassForState` returns `true`. This routes `CallOriginalPresent` through the bypass trampoline instead of calling `oPresent` (dxgi!Present with Steam's E9 JMP), which would re-enter Steam's overlay handler and crash because `vtable[8] = DetourPresent`.
- A safety net in `CallOriginalPresent` fallback path also handles this case directly: when `!slLoaded && presentBypass && IsSteamOverlayModule`, the bypass trampoline is used.
- Source anchors: `hook/common/dxgi_shared.h:239-244`, `hook/common/dxgi_shared.cpp:3111-3128`.
- Regression test: `SteamDX12BypassForNonSLSteamOverlay` in `tests/test_dxgi_shared.cpp`.

### Build 0.1.2906 — Fix: don't invoke Steam overlay hook from forced-bypass path without Streamline
- **Problem**: `CallOriginalPresent`'s forced-bypass block (line 3035) called `TryInvokeGuardedExternalSteamOverlayPresent` for ALL cases where `ShouldForceSteamDX12Bypass` returned true, including the non-Steam-overlay-without-Streamline scenario. But Steam's overlay handler crashes when invoked without Streamline on the stack because:
  - CE uses vtable hooking (vtable[8] = `DetourPresent`)
  - Steam's handler tries to find the "next" real Present by reading vtable[8]
  - Gets `DetourPresent` → can't resolve a valid handler → calls through NULL → RIP=0
- **Fix at that time**: Added `if (slLoaded)` around `TryInvokeGuardedExternalSteamOverlayPresent` in `CallOriginalPresent`. The old claim that a Streamline-stack guard alone made direct invocation safe is superseded: current code also requires the verified source Present thread and bypasses runtime workers.
- Also added improved debug logging: explicit "skipping Steam overlay invoke" log and updated "forcing DXGI bypass" log to include `slLoaded` state.
- **Source anchors**: `hook/common/dxgi_shared.cpp:3035-3055` (call-site fix with `slLoaded` guard + logging), `tests/test_dxgi_shared.cpp` (regression test `StrangeBrigadeSteamOverlayCrashWithoutStreamline`).
- **Edge cases covered**: (a) NvPresent loaded without Streamline also benefits from the same fix, (b) no bypass trampoline case unchanged (fundamental failure), (c) inline hook path (trampoline exists) unchanged.

### Build 0.1.2920 — Missing VirtualProtect around vtable[8]/[22] fixup (Strange Brigade crash regression)

- **Problem**: Strange Brigade DX12 with Steam overlay (no Streamline/FG) crashes on first Present with `0xC0000005` (AV-WRITE) at `vtable[8]`. The game never renders, CE overlay never appears.
- **Root cause**: The vtable[8]/[22] fixup code introduced in build 0.1.2908 writes to the swapchain vtable **without `VirtualProtect`**. CE's `InstallPresentInlineHooks` made the vtable writable, wrote hooks (`DetourPresent`/`DetourPresent1`), then restored the page to read-only. When the fixup code later writes `oPresentBypass` to vtable[8], it crashes on the read-only page. Every other vtable write site in the file uses `VirtualProtect`.
- **Fix** (`hook/common/dxgi_shared.cpp`):
  1. `CallOriginalPresent` (lines 3077-3086): Wrap vtable[8] save/write/restore with `VirtualProtect(PAGE_READWRITE)`/restore. If `VirtualProtect` fails, fall through to bypass trampoline.
  2. `CallOriginalPresent1` (lines 3263-3274): Same for vtable[22].
- **Regression test**: `CallOriginalPresentVtableFixupRequiresVirtualProtect` in `tests/test_dxgi_shared.cpp` validates the pattern on a read-only simulated vtable page.
- **Source anchors**: `hook/common/dxgi_shared.cpp:3076-3102`, `:3268-3297`, `tests/test_dxgi_shared.cpp:2536-2619`.
- **Stale-risk**: Low. VirtualProtect pattern matches all other vtable write sites; regression test catches removal.

### Build 0.1.5914 — locked slot reads fault on the read-only class vftable (20260811_192706 crash fallout)

- **Problem**: Session `logs/20260811_192706` (build 0.1.5914): all CE crash
  dumps (`dx12_fg_switch_test.exe` twice, Talos) plus the UE external minidump
  die identically at `RepairVTableHooksIfNeeded::<lambda0>` — `lock cmpxchg
  [rdx],r15` (0xC0000005 AV-WRITE) on `dxgi!CDXGISwapChain`'s class vftable
  inside the dxgi image (read-only `.rdata`). The game stack runs
  `sl_dlss_g!DllMain → DetourCreateSwapChainGlobal →
  RefreshPresentHooksForRealSwapchain → RepairVTableHooksIfNeeded`.
- **Root cause**: commit e9fa1341 replaced plain slot observations with
  `InterlockedCompareExchangePointer(slot, nullptr, nullptr)`.
  `lock cmpxchg` requires WRITE access even when only the result is used as a
  read, so it faults on the read-only class vftable between VirtualProtect
  windows — the same invariant family as the 0.1.2920 incident, now violated
  on the observation path. The same latent pattern was introduced in
  `DetachOwnedVTableSlot` and the Steam phase-A vtable[8] save in
  `CallOriginalPresent` (`dxgi_shared_original.cpp`).
- **Fix** (`hook/common/dxgi_shared_hooks_present.cpp`,
  `hook/common/dxgi_shared_original.cpp`): slot observation is a plain
  volatile read again in `repairRestoredSlot`, `DetachOwnedVTableSlot`, and
  the Steam vtable save. Atomic compare-exchange writes stay inside the
  existing VirtualProtect regions; CAS still preserves a concurrent foreign
  replacement.
- **Invariant**: never run a locked operation (cmpxchg/exchange) on a DXGI
  class-vftable slot outside a VirtualProtect(PAGE_READWRITE) region — a
  locked op is a write even when its result is discarded. Observation = plain
  volatile read; mutation = VirtualProtect + CAS + restore.
- **Regression test**: `tests/test_dxgi_shared_part13.cpp`
  (`DXGISharedVTableRepairTest.RepairReclaimsRestoredSlotsOnReadOnlyClassVftable`
  and `DetachRestoresOwnedSlotsOnReadOnlyClassVftable`) runs both paths
  against a VirtualAlloc'd fake vtable locked to PAGE_READONLY; pre-fix the
  unit suite exits 0xC0000005, post-fix both tests pass.
- **Stale-risk**: Low. The invariant is enforced by the regression tests; the
  0.1.2920 and 0.1.5914 incidents both stem from forgetting that the class
  vftable page is read-only outside repair/detach windows.

### Build 0.1.5917 — external-chain trampoline transport runs under the Steam NULL-callback guard (20260811_195131)

- **Problem**: Talos session `logs/20260811_195131` (build 0.1.5914): starting
  with DLSS FG active works, but switching DLSS FG -> FSR FG crashes on the
  fresh FSR swapchain. `hook_debug.log` shows CE's inline-hook trampoline
  (`S:3860 trampoline path=...`) calling straight into Steam's chain, and the
  install-time external `E9` (preserved `dxgi!Present` entry jump) is intact.
  When CE prepends over an external overlay's `E9`/`FF25` entry, the trampoline
  does not hold original code bytes - it re-issues the foreign entry jump, so
  `CallOriginalPresent`'s bare trampoline fast-path re-enters
  `gameoverlayrenderer64!OverlayHookD3D3`. On the fresh FSR swapchain Steam's
  lazy NULL rendering callback faults there with no VEH recovery active - the
  one Steam transport that had never been guarded.
- **Fix** (`hook/common/dxgi_shared_steam.cpp`, `dxgi_shared_original.cpp`,
  `dxgi_shared_internal.h`):
  1. `TrampolineChainsToExternalOverlay(trampoline, externalHook)` recognizes
     an `E9` or x64 `FF25` entry jump at the trampoline start and either
     matches the preserved external hook target
     (`dxgi_shared_g_externalOverlayPresentHook`) or, without a preserved
     target (Present1), accepts any chain target outside the dxgi image -
     the same rule as install-time external-jump detection.
  2. `IsSteamExternalChainTrampoline(...)` gates that to D3D12 swapchains with
     the Steam overlay module loaded.
  3. In `CallOriginalPresent`, that transport now runs under
     `TryInvokeGuardedExternalSteamOverlayPresent` (NULL-callback VEH +
     proactive slot patch + source-thread provenance, same as every other
     Steam invoke) and fails closed to the clean DXGI bypass. During shutdown
     (no VEH recovery) the bypass is used directly.
  4. `CallOriginalPresent1` has no Present1-specific guard, so a Steam-chain
     Present1 trampoline uses the clean Present1 bypass, falling back to the
     guarded Present transport.
- **Invariant**: no Steam transport may run bare. Every path that can
  re-enter Steam's hook chain must carry the NULL-callback VEH guard or use
  the clean bypass - including the inline-hook trampoline when CE prepended
  over Steam's entry jump.
- **Regression tests**: `tests/test_dxgi_shared_part13.cpp`
  (`DXGISharedSteamTrampolineChainTest`: FF25/E9 chain matching, generic
  foreign-target mode, clean-trampoline and null-argument rejection);
  `tests/test_dxgi_shared_part11.cpp` source-order guard
  (`SteamExternalChainTrampolineNeverCalledBareBeforeGuardedTransport`).
- **Stale-risk**: Low-Medium. The trampoline layout is CE-owned and the
  install-time detection rule is shared, but Steam builds keep moving; the
  VEH guard remains the backstop for unknown slot/build shapes.

### Build 0.1.5921 — the dedicated overlay queue is disabled for NVIDIA DLSS FG in every detection state (late-inject Alt+Tab crash 20260811_214252)

- **Problem**: Talos session `logs/20260811_214252` (build 0.1.5919):
  CaptureEngine was started while Talos was already running with DLSS FG
  suspended; Alt+Tabbing back into the game resumed DLSS FG and the game
  crashed. UE log: `Streamline/DLSSG present failed ... DXGI_ERROR_DEVICE_REMOVED
  with Reason: 887A002B` immediately after `Engaging WAR4639162`; the UE
  minidump stack ends in `sl.dlss_g` raising `STATUS_FATAL_APP_EXIT`.
- **Root cause**: late injection loads `capture_hook_x64.dll` after
  `sl.dlssg`/`sl.interposer` already exist, so CE never hooks their exports:
  `g_StreamlineFGRunning` stays false and the runtime-ownership latch never
  fires (`slFG=0`, `ownership=0` in the FG-transition log). The FG planner
  still classifies `DLSS_FG` through the NVNGX `CreateFeature` hook, so at FG
  resume `EnsureDedicatedOverlayQueueForFGCompat` saw "FG active" with no
  dedicated queue and forced a sync reinit. The warm overlay backend records
  normal-route command lists that draw DIRECTLY to the swapchain backbuffer;
  the first submit of such a list on the dedicated (non-owning) queue returns
  `DXGI_ERROR_ACCESS_DENIED (0x887A002B)` and removes the device - the exact
  failure mode documented in `overlay_compat_detail/routing_policy.h`
  (`logs/20260606_153428`). Healthy startup sessions never hit it because at
  DLSS activation at least one of the Streamline/runtime-owned latches is
  present and `ShouldUseDedicatedOverlayQueue()` already returned false.
- **Fix**:
  1. `ShouldDisableDedicatedOverlayQueueForNvidiaFrameGeneration(streamlineFGRunning,
     runtimeMode)` (new predicate in `fg_metrics_and_transitions.h`) disables
     the dedicated queue when the Streamline latch is set OR the planner
     runtime mode is `kDLSSFG`; `ShouldUseDedicatedOverlayQueue()` now uses it.
     The FG-resume reinit therefore stays single-queue on the live present
     queue exactly like the healthy startup paths, and the overlay keeps
     drawing through the transition (no reinit, no blank).
  2. Defense in depth: `ShouldUseDedicatedQueueForOverlaySubmit(...)` reserves
     the dedicated queue for pure-offscreen lists. `DrawSubmitCoreTail` always
     passes `recordedListTouchesBackbuffer=true` (the ProcessFrame list draws
     or copies the backbuffer in every route), and `SubmitOverlayCommandList`
     gained a `listTouchesBackbuffer` parameter (only caller: startup resource
     priming, which is font-texture upload only and passes false). A future
     policy state that again allows the dedicated queue cannot resurrect the
     device removal because the submit sites fail closed to the game queue.
- **Non-regression**: games without FG never enabled the dedicated queue
  (`actualFGActive=false`); FSR FG and healthy DLSS FG were already disabled
  through `fsrFGActive`/runtime-ownership/Streamline latches. The retained
  session history shows the dedicated queue was created only in the crashing
  session.
- **Regression tests**: `tests/test_dxgi_shared_part14.cpp`
  (`DedicatedOverlayQueueDisabledForNvidiaDLSSFrameGeneration`,
  `DedicatedOverlayQueueSubmitRequiresOffscreenList`,
  `DedicatedOverlayQueueSubmitGuardsBackbufferLists` source invariant).
- **Stale-risk**: Low. The invariant is enforced by unit tests; the remaining
  risk is a future FG runtime that changes the swapchain queue-association
  rules, which the submit-time guard would surface as a forced game-queue
  fallback with a rate-limited `Dedicated overlay queue bypassed for
  backbuffer-touching ... submit` log.

### Build 0.1.5922 — route late-inject DLSS-FG overlay submits to the swapchain-owning queue (20260811_221202)

- **Problem**: session `logs/20260811_221202` (build 0.1.5921) still crashed
  with the identical UE fatal (`Streamline/DLSSG present failed ... Reason:
  887A002B`) although the 0.1.5921 dedicated-queue guard was active and logged
  `Dedicated overlay queue disabled for NVIDIA DLSS FG`. The `FG overlay
  SUBMIT #1` went to `gameQ=1` (the game queue) on `000001958621A0C0` and
  still removed the device - the dedicated queue was NOT the trigger.
- **Corrected root cause**: `0x887A002B` is `DXGI_ERROR_ACCESS_DENIED`
  (verified against the Windows SDK `winerror.h`): the swapchain backbuffer
  may only be drawn from the queue the swapchain was created with. Talos uses
  separate render/present DIRECT queues; at DLSS-FG resume the game's ECL
  traffic moves to the DLSS-G render queue (`g_CommandQueue` flips away from
  `origGame`, the swapchain owner). With the Streamline latch missing under
  late injection, `DecideSwapchainOverlayRouting` fell through all FG branches
  to the generic fallback (`scQueue ?: last ECL queue`) and submitted the
  overlay's backbuffer-drawing list on the render queue. The SL-latched
  healthy path routes pure DLSS to `origGame` (`kUseStreamlineOriginalQueue`),
  which is why startup injection and the retained healthy sessions never hit
  this.
- **Fix**: `DecideSwapchainOverlayRouting` gained `plannerDLSSFGActive`;
  `IsDLSSFrameGenerationActive()` (planner `kDLSSFG`) is passed by both call
  sites (`dx12_hook_process_session_phase2.cpp` ProcessFrame queue resolution
  and `dx12_hook_overlay.cpp` InitOverlaySync backend selection), and the two
  Streamline branches (`hadFSRFGPhase` and pure-DLSS) treat planner-classified
  DLSS exactly like the SL latch. The late-inject FG-resume draw therefore
  lands on `origGame` (the swapchain owner) instead of the DLSS-G render
  queue. Non-FG, FSR, and SL-latched routing is unchanged (parameter defaults
  to false; all other branches untouched).
- **Non-regression**: games without FG never enter the new branches
  (`plannerDLSSFGActive=false`); the dedicated-queue guards from 0.1.5921
  remain as defense-in-depth; Steam transport rules are untouched.
- **Regression tests**: `DX12SwapchainOverlayRoutingTreatsPlannerDLSSLikeStreamlineLatch`
  in `tests/test_dxgi_shared_part3.cpp` (planner-DLSS-without-latch routes to
  `kUseStreamlineOriginalQueue`; non-DLSS state still falls through to normal
  routing; the SL latch still dominates).
- **Stale-risk**: Low-Medium. The routing decision is covered by unit tests,
  but queue-ownership shape can differ per engine; the `ProcessFrame queue=`
  diagnostic with `path=origGame` during DLSS FG plus a healthy
  `devRemoved=0x00000000` on `FG overlay SUBMIT` is the runtime proof to watch
  in Talos late-inject validation.

### Build 0.1.2908 (SUPERSEDED by 0.1.2922) — Steam overlay visible: invoke directly with vtable[8] fixup (non-SL case)
- **Problem**: The 0.1.2906 fix prevented the crash but also made Steam overlay permanently invisible in the non-Streamline case. The bypass trampoline jumped over Steam's E9 JMP entirely.
- **Original fix** (`hook/common/dxgi_shared.cpp`): In `CallOriginalPresent`'s forced-bypass block, when `slLoaded=0`, invoke Steam's overlay handler directly with vtable[8] fixup:
  1. Save vtable[8], set it to the bypass trampoline (valid forwarding target)
  2. Increment `s_externalOverlayPresentInvokeDepth` (activates recursion guard)
  3. Call `g_externalOverlayPresentHook` directly
  4. Restore vtable[8], decrement depth
  5. Return Steam's HRESULT
- **WHY SUPERSEDED**: The vtable[8] fixup + direct Steam handler call approach incorrectly assumed Steam reads vtable[8] from the swapchain object at call time. In practice, Steam's OverlayHookD3D3 caches the "next" Present handler pointer INTERNALLY when its E9 JMP is first triggered through the natural dxgi!Present entry point. Since CE uses vtable hooking (bypassing Steam's E9 JMP on dxgi!Present), the cached pointer was never initialized and remained NULL → RIP=0 crash on first Present even with the vtable[8] fixup. Replaced by oPresent (E9 JMP) routing in build 0.1.2922.
- **Source anchors**: Same files, superseded by 0.1.2922 code.
- **Stale-risk**: SUPERSEDED. Do not restore the vtable[8] fixup + direct Steam handler call approach.

### Build 0.1.2922 (SUPERSEDED by 0.1.2923) — Steam overlay via oPresent (E9 JMP) routing for non-SL Steam overlay (Strange Brigade DX12 fix)
- **Problem**: Strange Brigade DX12 with Steam overlay (no Streamline, no FG) crashed on first Present with `0xC0000005` (RIP=0) inside Steam's OverlayHookD3D3.
- **Root cause (superseded by 0.1.2923 analysis)**: The 0.1.2922 analysis incorrectly assumed Steam's OverlayHookD3D3 reads vtable[8] and successfully uses DetourPresent as a forwarding target. In reality, Steam lazily initializes its internal "next" Present handler on first E9 JMP entry by reading vtable[8], and the initialization VALIDATES the pointer — if it points to anything other than the real dxgi!Present (e.g. DetourPresent), Steam's validation fails and sets "next" = NULL → RIP=0 crash.
- **Fix**: Routing through `oPresent` (dxgi!Present with Steam's E9 JMP) with the expectation that Steam would initialize its internal pointer from vtable[8] (= DetourPresent) and call it back into DetourPresent's reentrancy guard.
- **WHY SUPERSEDED**: The 0.1.2922 approach crashes because Steam's OverlayHookD3D3 does NOT accept DetourPresent as a valid "next" handler during initialization. Steam reads vtable[8], finds DetourPresent, validation fails, sets "next" = NULL, and the crash occurs inside Steam's overlay code before any reentrancy guard can catch it. The oPresent → DetourPresent → reentrancy guard → bypass chain never completes because Steam's internal init fails immediately on first entry. Replaced by the one-time vtable unhook approach in build 0.1.2923.
- **Source anchors**: `hook/common/dxgi_shared.cpp:3063-3118` (original fix), `:3254-3304` (original Present1 fix).
- **Stale-risk**: SUPERSEDED. Do not restore the oPresent routing approach — it relies on an incorrect assumption about Steam's initialization mechanism.

### Build 0.1.2923 (SUPERSEDED by 0.1.2928) — One-time vtable[8] unhook for Steam DX12 overlay init (Strange Brigade DX12 fix — DID NOT WORK)
- **Problem**: Strange Brigade DX12 with Steam overlay active (no Streamline, no DLSS FG, no FSR FG) crashes on the first Present call with `0xC0000005` (RIP=0) inside Steam's OverlayHookD3D3.
- **Incorrect root cause (build 0.1.2923 analysis)**: Assumed Steam's OverlayHookD3D3 reads vtable[8] to find a "next" handler on first E9 JMP entry, and that DetourPresent fails validation → "next" = NULL → crash.
- **Fix that did NOT work**: Temporarily restored vtable[8] to dxgi!Present, called through E9 JMP, re-hooked. The fix IS executing (confirmed by hook_debug.log) but the crash STILL happens.
- **WHY SUPERSEDED**: cdb disassembly of the crash dump proved the crash is from a **NULL global function pointer** in Steam's overlay data section at RVA ~0x1621d8 in gameoverlayrenderer64.dll, NOT from reading vtable[8]. The instruction at `OverlayHookD3D3+0x1417f` loads `rax = qword ptr [VulkanSteamOverlayProcessCapturedFrame+0x9b378]` (rax = 0 = NULL) and calls `rax` without NULL check. This is an internal Steam overlay rendering callback that was never initialized. Replaced by temp swapchain pre-init in build 0.1.2928.
- **Stale-risk**: SUPERSEDED. The vtable[8] unhook approach alone does NOT fix the crash.

### Build 0.1.2928 (SUPERSEDED by 0.1.2930) — Pre-init Steam overlay on temp swapchain during hook installation (DID NOT WORK)
- **Problem**: Same as builds 0.1.2922/0.1.2923 — still crashing.
- **Root cause**: Same NULL global function pointer at RVA 0x1621d8.
- **Fix that did NOT work**: Called `vtable[8](pSwapChain, 0, 0)` on the detection temp swapchain BEFORE setting vtable[8] = DetourPresent.
- **WHY it didn't work**: The temp swapchain is 2×2 with a hidden window. Steam's OverlayHookD3D3 skips its rendering initialization path when the swapchain is not a "real" game swapchain (no visible window, no buffers). The callback at RVA 0x1621d8 remained NULL. The temp swapchain pre-init did initialize Steam's "next" handler but NOT the rendering callback.
- **Stale-risk**: SUPERSEDED. The temp swapchain pre-init alone is insufficient.

### Build 0.1.2930 — VEH-protected Steam overlay init on game swapchain (Strange Brigade DX12 fix)
- **Problem**: Same — Strange Brigade DX12 crashes on first Present with RIP=0 at `call rax` (RAX loaded from RVA 0x1621d8 = NULL).
- **Root cause** (confirmed by cdb disassembly of crash dump `crash_20260508_192218_047_pid8308_tid23364.dmp`):
  ```
  f11a533e  mov rax,qword ptr [VulkanSteamOverlayProcessCapturedFrame+0x9b378]  ; RAX = [base+0x1621d8] = 0
  f11a5345  mov r8d, ebp                                                          ; arg3 = flags
  f11a5348  mov edx, esi                                                          ; arg2 = 0
  f11a534a  mov rcx, r14                                                          ; arg1 = swapchain
  f11a534d  call rax                                                              ; call through NULL → crash
  ```
  - The NULL function pointer at RVA `0x1621d8` in gameoverlayrenderer64.dll is an internal Steam overlay rendering callback.
  - CE's vtable hook (vtable[8] = DetourPresent) prevents Steam's E9 JMP from ever firing on a real game swapchain, so the callback never gets initialized.
  - The temp swapchain pre-init (build 0.1.2928) doesn't fix this because Steam only initializes the callback when rendering on a real game swapchain with a visible window.
  - The vtable unhook safety net (build 0.1.2923, AttemptSteamDX12OverlayInit) also crashes because it calls through the E9 JMP which triggers Steam's overlay to try to render → NULL callback → crash.
- **Fix** (`hook/common/dxgi_shared.cpp`):
  1. Added `SteamDummyRenderingCallback` — a no-op callback that returns `S_OK`, serving as a safe placeholder.
  2. Added `SteamOverlayInitVehHandler` — a VEH handler that catches the NULL callback crash (RIP=0, RAX=0, return address in gameoverlayrenderer64):
     - Identifies the crash by checking RIP=0, RAX=0, and return address inside gameoverlayrenderer64.dll
     - Patches the NULL pointer at RVA 0x1621d8 to `SteamDummyRenderingCallback`
     - Fixes the context: pops the stale return address from stack, sets RAX to the dummy, sets RIP back to the `call rax` instruction
     - Returns `EXCEPTION_CONTINUE_EXECUTION` so the `call rax` retries with a valid pointer
  3. Modified `AttemptSteamDX12OverlayInit` — wraps the `presentOriginal(pSwapChain, 0, 0)` call with VEH: installs `SteamOverlayInitVehHandler` before the call and removes it after.
  4. Updated comments in `InstallPresentInlineHooks` to note that the temp swapchain pre-init only initializes Steam's "next" handler, not the rendering callback (the VEH fix in AttemptSteamDX12OverlayInit handles the real problem).
- **How it works**:
  1. First non-SL Present call → `CallOriginalPresent` → `AttemptSteamDX12OverlayInit`
  2. vtable[8] restored to dxgi!Present
  3. VEH handler installed
  4. Called through E9 JMP (`presentOriginal`) → Steam fires → hits NULL callback → crash
  5. VEH handler catches the crash, patches RVA 0x1621d8 to `SteamDummyRenderingCallback`, retries
  6. `call rax` retries with valid pointer → dummy executes → returns S_OK → Steam continues
  7. Steam completes its overlay processing → Present returns to AttemptSteamDX12OverlayInit
  8. VEH handler removed, vtable[8] re-hooked to DetourPresent
  9. Subsequent frames route through E9 JMP normally — callback is no longer NULL (Steam may overwrite the dummy with its own function during the first E9 JMP entry, or the dummy stays as a safe no-op that prevents crashing)
- **Architecture support**: The VEH handler is compiled for both x64 and x86 (uses `#ifdef _WIN64` for Rip/Rax/Rsp vs Eip/Eax/Esp register names, and different Steam DLL names).
- **Source anchors**: `hook/common/dxgi_shared.cpp:305-381` (dummy callback + VEH handler), `:3236-3238` (VEH-wrapped call in AttemptSteamDX12OverlayInit).
- **Verification**: All 696 unit tests pass build 0.1.2930.
  8. Clean flow: DetourPresent → CallOriginalPresent → oPresent → Steam overlay → real Present. No re-entrancy into DetourPresent.
- **Why this is safe**:
  - One-time setup: The vtable unhook/re-hook happens only on the very first non-SL Steam overlay Present call
  - VirtualProtect is used for all vtable writes (matches existing pattern from all other vtable write sites)
  - If Steam's E9 JMP is NOT present (oPresent == dxgi!Present), the init call just calls real Present directly — no harm, no Steam overlay
  - `s_steamInitCrashed` flag provides crash-safe fallback: if init crashes (e.g. Steam overlay removed at runtime), subsequent calls use bypass trampoline
  - Thread safety: `s_steamDX12InitAttempted` is atomic. Only one thread performs the init. Other threads arriving during init wait for `s_steamDX12InitAttempted = true` and will use bypass if init crashed.
  - No re-entrancy into DetourPresent: after init, Steam's "next" handler points to real Present, not DetourPresent
  - No stack overflow: at most one level of reentrancy during the init phase (DetourPresent → CallOriginalPresent → init helper → oPresent → Steam → real Present → return)
- **Fallback**: If `AttemptSteamDX12OverlayInit()` crashes (Steam overlay removed or init fails silently), `s_steamInitCrashed = true` and all subsequent calls use bypass trampoline (Steam overlay not visible, but game doesn't crash).
- **Source anchors**: `hook/common/dxgi_shared.cpp` (AttemptSteamDX12OverlayInit, CallOriginalPresent init block), `tests/test_dxgi_shared.cpp` (SteamDX12InitVtableUnhookRestorePattern, SteamDX12InitVtableRehookFailureSafety).
- **Verification**: All unit tests pass. Regression tests cover the VirtualProtect unhook → call → re-hook pattern on read-only vtable pages (SteamDX12InitVtableUnhookRestorePattern) and safe behavior if re-hook fails (SteamDX12InitVtableRehookFailureSafety).

### Build 0.1.2948 — vtable[8] restore before Steam overlay invoke (Strange Brigade DX12)

- **Problem**: Build 0.1.2947 bypass-only confirmed working (game content + CE overlay, no Steam overlay). Need to re-enable Steam overlay without black screen.
- **New root cause hypothesis**: Steam's DX12 overlay handler (`gameoverlayrenderer64!OverlayHookD3D3`) may internally call `pSwapChain->Present()` as part of its hook chain protocol (e.g., post-overlay fence wait and Present sequencing). With vtable[8] = `DetourPresent` (CE's vtable hook), such internal Present calls re-enter CE → `CallOriginalPresent` → either explicit Steam invoke (recursive) or bypass trampoline. The bypass trampoline skips Steam's E9 JMP entirely, breaking Steam's expected "next" handler chain. Steam's overlay commands may be submitted but never properly sequenced with the Present call, leading to buffer corruption.
- **Fix**: Before invoking `TryInvokeGuardedExternalSteamOverlayPresent`, temporarily set vtable[8] back to the original `dxgi!Present` (which has Steam's E9 JMP). After Steam's handler returns, re-hook to `DetourPresent`. This ensures Steam's internal Present calls flow through the natural E9 JMP → Steam handler (re-entrant) → Steam's saved "next" → real Present body.
- **Invariant**: The vtable[8] restore/re-hook window is per-frame and microsecond-scale. If vtable[8] was modified by another component during Steam's handler execution, the re-hook is skipped (logged).
- **Fallback**: If `TryInvokeGuardedExternalSteamOverlayPresent` is declined (guard conditions not met), use bypass trampoline which preserves game content + CE overlay but disables Steam overlay.
- **Source anchors**: `hook/common/dxgi_shared.cpp:3507-3537` (Phase A: vtable restore), `3540-3562` (Phase B: Steam invoke), `3565-3601` (Phase C: vtable re-hook), `3608-3619` (Phase E: bypass fallback).
- **Pending test**: Strange Brigade DX12 with Steam overlay. Check all three outcomes: (1) all overlays visible, (2) bypass fallback with CE+game visible, (3) black screen (further analysis needed).

### Build 0.1.2960-2963 — ECL-hook-based deferred CE overlay submission (Strange Brigade DX12 black screen fix)

- **Problem**: ALL approaches to invoking Steam's DX12 overlay handler (E9 JMP, explicit invoke, vtable restore, Steam-only experimental) produce black game content. Build 0.1.2949 diagnostic logging confirmed Steam's handler ONLY submits an ECL to the game queue — no Present calls, no buffer modifications, no code byte changes. The hypothesis is that Steam's overlay ECL clears/overwrites the backbuffer, erasing both game content and CE's overlay ECL (submitted before Steam invoke via `ProcessFrame`).
- **The ECL-hook approach**: Defer CE overlay ECL submission to `DetourExecuteCommandLists`, which fires AFTER Steam's overlay ECL on the same queue. Queue order: [Steam ECL] [CE overlay ECL] [fence signal] [Present].
- **Implementation**:
  1. `g_deferOverlaySubmitToSteamECL` flag — set by `DetourPresent` for non-SL Steam path
  2. `g_steamDeferredOverlay` state struct — captures cmdList, allocIdx, eclQueue, pending
  3. `ProcessFrame` — when flag set, records overlay commands and closes list but skips ECL submission + fence signal (jumps to `skip_steam_deferred_fence_signal`)
  4. `SubmitSteamDeferredOverlay` — submits deferred ECL via `g_RealD3D12ECL` or `GetOriginalExecuteCommandLists` (per-queue original, avoids recursion into `DetourExecuteCommandLists`), signals fence
  5. `DetourExecuteCommandLists` — detects Steam overlay ECL via `IsSteamOverlayModulePath("gameoverlayrenderer")`, calls `SubmitSteamDeferredOverlay(pThis, "ecl_hook")` after original ECL completes
  6. `DetourPresent` — skip normal fence wait before Present when deferred; after `CallOriginalPresent`, submit fallback if still pending (Steam never called ECL), then wait fence, clear flag
  7. Non-hook build stubs in `dxgi_shared.cpp` — `ResolveDX12SetDeferOverlay`, `ResolveDX12SubmitSteamDeferredOverlay`, `ResolveDX12IsDeferOverlayPending` via `GetModuleHandleA`/`GetProcAddress`
- **Key design decisions**: Automatic (no env vars), non-SL only (`steamOverlayLoaded && !IsSLInterposerLoaded()`), `GetOriginalExecuteCommandLists` over vtable fallback, fallback safety for frames where Steam doesn't call ECL.
- **Diagnostic logging**: "non-SL Steam path — deferring overlay" with SyncInterval/Flags, "Deferring overlay ECL submit to Steam ECL hook #N", "ECL hook detected Steam with deferred overlay pending" vs "no deferred overlay pending", "Submitting Steam-deferred overlay ECL to queue %p (cmdList=%p, allocIdx=%d)", "Deferred overlay submitted #N (queue=%p, fence=%llu)", fallback submit log, "Post-Steam fence wait took X us (wasPending=%d)", fence-already-complete with mode info.
- **Source anchors**: `hook/apis/dx12_hook_main.cpp` (~line 955-1080: state struct, exports, SubmitSteamDeferredOverlay, IsSteamOverlayModulePath, ProcessFrame skip logic, DetourExecuteCommandLists ECL hook detection), `hook/common/dxgi_shared.cpp` (~line 1970-2150: deferral flag set, skip fence wait, post-CallOriginalPresent fallback + fence wait + clear).
- **Verification**: Build 0.1.2963 compiles, all 696 unit tests pass.
- **Open questions / stale-risk**:
  - ECL hook fires ~1 in 50+ frames — fallback path submits after Present, too late for current frame
  - Even when ECL hook DOES fire, black screen may persist — root cause may differ from ECL backbuffer clearing
  - No D3D12 debug layer / GPU validation active — potential resource state mismatches between Steam's ECL and CE's overlay ECL go uncaught
  - Steam's ECL may leave backbuffer in non-PRESENT state, making CE's `PRESENT→RT` barrier incorrect
- **Pending**: Strange Brigade DX12 testing with build 0.1.2963.

### Build 0.1.2947 — Black screen: ALL Steam handler invoke approaches fail — bypass-only fallback (Strange Brigade DX12)

- **Problem**: Build 0.1.2943 (explicit Steam overlay invoke) still produces black game content. Log confirms `TryInvokeGuardedExternalSteamOverlayPresent` IS called and invokes Steam's handler (`hook=00007FFF8725058A`), but the frame is black.
- **Root cause (refined)**: Three different approaches to invoking Steam's overlay handler ALL produce black game content:
  1. E9 JMP path: calling `dxgi!Present` (with Steam's E9 JMP)
  2. Explicit invoke: calling `g_externalOverlayPresentHook` (Steam's handler directly)
  3. Vtable restore: temporarily setting vtable[8] = dxgi!Present, calling Present, re-hooking

  Even when Steam's overlay renders nothing and just calls "next" to present, the game content on the backbuffer is lost. The most likely cause: Steam's init Present call in `AttemptSteamDX12OverlayInit` alters the GPU buffer state on the real game swapchain, changing how CE's subsequent PRESENT→RT barrier behaves. Some GPU drivers may discard/clear backbuffer contents on PRESENT→RT transition when the buffer's internal state tracking was modified by Steam's init.
- **Current approach**: Bypass-only for non-SL Steam path. The bypass trampoline calls `dxgi!Present+5` directly, skipping both CE vtable hook and Steam E9 JMP. Game content + CE overlay visible, but NO Steam overlay.
- **Pending solutions (to explore)**:
  - **Chain inline hook**: Install inline hook on `dxgi!Present` WITH chain to Steam's E9 JMP, avoiding vtable modification entirely. Risk: CE and Steam fight over `dxgi!Present` entry bytes.
  - **PE-read COM method**: Read original `IDXGISwapChain::Present` COM method from `dxgi.dll` PE `.rdata` section, find the method that does kernel state management before calling `dxgi!Present`.
  - **Skip CE overlay when Steam is active**: Let Steam own the Present call, render CE overlay separately via a different GPU queue or post-present mechanism.
  - **Separate overlay device/queue**: Create a separate D3D12 device and command queue for CE overlay rendering that doesn't touch the game swapchain buffers; composite via shared textures.
- **Source anchors**: `hook/common/dxgi_shared.cpp:3499-3515`.
- **Verification**: All 696 unit tests pass. Build 0.1.2947.

### Build 0.1.2943 — Black screen fix (corrected): explicit Steam overlay invoke instead of E9 JMP — s_originalVtable8Present was a no-op (Strange Brigade DX12)

- **Problem**: Build 0.1.2942 (with the vtable[8] COM method fix) still produced black screen. Log confirmed `s_originalVtable8Present=00007FFF86A4D9F0 (same=1)` — the saved vtable[8] was identically `dxgi!Present` (the inner function). **There is no separate COM method.** The build 0.1.2941 fix was a complete no-op.
- **Corrected root cause**: For this DX12 game, vtable[8] IS `dxgi!Present` (the inner function) — there is no COM wrapper method. Both `s_originalVtable8Present` and `presentOriginal` (= `oPresent`) point to the same address. Calling `dxgi!Present` (with Steam's E9 JMP) from `CallOriginalPresent` enters Steam's overlay handler via the inline JMP hook. The internal path Steam takes when entered through its E9 JMP on the function body differs from the path taken when invoked as a standalone hook target (`g_externalOverlayPresentHook`). The E9 JMP entry path produces black game content — the exact GPU-level mechanism is complex but confirmed by repeated testing.
- **Fix**: Replace the `s_originalVtable8Present` / E9 JMP path with `TryInvokeGuardedExternalSteamOverlayPresent`. This calls Steam's overlay handler directly via `g_externalOverlayPresentHook` (the resolved E9 JMP target, saved during `InstallPresentInlineHooks` at line 2992-2996). Steam renders its overlay, calls "next" (original dxgi!Present body or re-entrant DetourPresent → bypass), and presents normally. CE's overlay submission + fence wait happens in `DetourPresent` before `CallOriginalPresent`.
- **Historical wrapper observation, not a nonblocking guarantee**: `CWrapDXGISwapChain::Present` sets `g_InWrapperPresent = false` before delegating to the detour hook. That only clears the wrapper-policy rejection; source-thread provenance is still mandatory whenever a runtime can Present from workers.
- **Fallback**: bypass trampoline (game content + CE overlay visible, Steam overlay dropped for that frame).
- **Source anchors**: `hook/common/dxgi_shared.cpp:3475-3516` (non-SL explicit Steam invoke), `dxgi_swapchain_wrap.cpp:916-920` (g_InWrapperPresent = false during delegation).
- **Verification**: All 696 unit tests pass. Build 0.1.2943.


## RESOLVED: native FSR FG put CE below Steam again — the deep body hook adds the topmost composite (2026-08-13, build 0.1.5999)

The 0.1.5960 rule fixed draw order for ordinary and DLSS-FG presents, but the native FSR FG app-callback route re-created the same symptom one layer deeper. Session `20260813_061015` (Talos + Steam overlay + official FFX FSR FG, build 0.1.5995) shows why: CE's FFX present callback composites the overlay into the runtime output buffer, the runtime presents that buffer through DXGI afterwards, and Steam's entry hook composites on top of it. CE's deep body hook below the foreign chain runs on that same present every frame — the `[OVERLAY LAYER] deep-body-below-foreign-chain` note fired — but `ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain` routed the actual draw away because the runtime-owned native-FSR path must normally keep CE's separate GPU work suppressed.

- **Fix**: `DecideBelowForeignChainFSRDeepDraw` + `DX12_CompositeOverlayBelowForeignChainForRuntimeOwnedFSR` (`hook/common/dx12_overlay_policy/ffx_routing.h`, `hook/apis/dx12_hook_ffx_owner_queue.cpp`, wired from `DrawSkipAndCounters` in `hook/apis/dx12_hook_process_session_draw_main.cpp`). When CE is below a foreign chain, at least one tracked overlay is loaded, the FFX present callback is live and un-stalled, and none of the no-callback/teardown/protected-startup/deviceremoved states apply, the deep body hook draws a second overlay composite onto the presented swapchain's exact current backbuffer on the swapchain-owning queue. In the repro that is the queue Steam's own ECL went through (`scQueue=000001A1FA0B1440`), so queue order is Steam -> CE and CE's overlay is topmost.
- **Why it is safe where 0.1.5970-0.1.5972 were not**: the draw uses `dx12_ffx_suspend_overlay` — the exact-current-buffer RTV per frame (the normal backend's preserved RTV heap can still reference the pre-FG buffers, the stale-target shape of the 0.1.5972 FG-off device removal), per-slot in-flight refusal without waiting/overwriting, and retained target resources until completion proof. It is the swapchain-owning queue (not the idle game queue of the 0.1.5970 save-load "never landed" case). The callback baseline remains until the first successful deep submit; each success arms exactly the next callback to yield, and that callback consumes the proof. If deep Presents stop or a submit is refused, no stale latch survives to suppress the following callback.
- **Route value**: `DX12OverlayRenderRoute::kBelowForeignChainRuntimeOwnedFSR` (`below-foreign-chain-runtime-owned-fsr`); the layering diagnostic is `[OVERLAY LAYER] ... site=deep-body-below-foreign-chain-runtime-owned-fsr ...`. The FG-UI-composition `[OVERLAY LAYER]` note no longer claims the callback is the only runtime-safe channel unconditionally.
- **Explicitly excluded**: no-callback internal composition (the documented ffxQuery wedge / 0x887A002B boundary), explicit FSR-off teardown, stalled callbacks (the existing stall fallback rules own that state), protected startup quiescence, DLSS/Streamline, and any frame where the routed queue is not the swapchain queue.
- **Tests**: `tests/test_ffx_below_foreign_chain_policy.cpp`. Verify gate passed on 0.1.5999. **Open**: needs the user's Talos FSR-FG + Steam run to confirm topmost layering and no device removal across the FG switch matrix; GTA's historical app-callback ACCESS_DENIED boundary must be re-checked there too.
- **Follow-up (2026-08-13, build 0.1.6000)**: the Talos FSR-FG + Steam validation run crashed on the FSR->DLSS
  menu switch with the game's own `WindowsD3D12Viewport.cpp:267` `80070005` fatal ensure, because the deep draw's
  renderer state is keyed by the PRESENTED FFX swapchain while the FFX teardown only retired the registered
  game-facing proxy states — the deep-draw command lists/backbuffer refs survived the FFX swapchain teardown.
  `RetireAllForNativeFSRTeardown` now retires every live suspend-overlay state at both the Streamline-enable prep
  and FFX context-destroy boundaries (`hook/apis/dx12_ffx_suspend_overlay.cpp`,
  `hook/apis/dx12_hook_fg_state.cpp`, `hook/apis/dx12_hook_ffx_owner_queue.cpp`); in-flight states stay retained
  until their own fence completes. See the guardrails invariant and `log/recent.md` for the dump evidence.

## VALIDATED: no-callback FSR learns the final GPU batch and joins it last (2026-08-14)

Session `installed/captureengine/logs/20260814_015902` is the distinct no-app-callback topology
(`internalNoCallback=1`). Proxy-Present prework correctly draws CE into the registered/substituted FFX UI texture
on its owner queue, but AMD composites that texture before the presenter thread's final DXGI work. Steam/RTSS then
submit later ECL batches before the deep system Present, so the UI-resource route is intrinsically below them.

- **Fix**: the no-callback ECL fast path records the presenter thread's immediate return address and ordinal for
  each non-CE `ExecuteCommandLists` batch. Two identical consecutive final signatures arm the next frame. At that
  exact batch, CE records against the exact current FSR backbuffer and calls the existing predecessor once with
  `[original lists..., CE list]`. CE is therefore last regardless of whether Steam, RTSS, ReShade, Special K,
  OptiScaler, or an unrecognized effect submitted the final batch; no product/module allowlist selects the route.
- **Safety boundary**: this is not the failed arbitrary/separate FSR submit. It adds no second ECL call and no queue
  `Signal`. `ID3D12GraphicsCommandList2::WriteBufferImmediate` writes a per-buffer completion marker at the list
  tail, protecting allocator/upload/target reuse without changing AMD's queue-operation cadence. Signature/thread/
  queue/target mismatch or pending slot reuse refuses the append without waiting.
- **Make-before-break handoff**: the proven FFX UI-resource route stays visible during learning. The first stable
  final-batch append is marker-only and does not create an RTV, transition, clear, or draw. After the marker completes,
  proxy prework clears a CE-owned substitute once and explicitly grants visible ownership; until that grant, every
  further append remains a non-visible probe. Revocation precedes UI-baseline resumption, closing the
  completion-between-prework-and-ECL race. Game-owned UI is never erased.
- **Hardware result / follow-up**: session `20260814_024908` (0.1.6049, Steam + RTSS) and the user's visual check
  confirm that CE is topmost under FSR FG. Session `20260814_032638` then isolated transition double blending: both
  handoffs now begin with non-visible probes, no-callback drawing additionally requires the post-proof UI-retirement
  grant, and every callback-routing edge synchronously clears both topmost epochs.
- **Remaining 0.1.6051 causes (`20260814_035452`)**: the no-callback completion query incorrectly required *all*
  latest inline markers to be complete. A freshly in-flight output therefore revoked already-proven ownership at
  `03:55:04.922`, briefly restoring the darker UI-resource baseline. Completion proof now latches for the current
  learned-signature generation, while 16 allocator/upload slots rotate independently and only their own marker
  guards reuse. Signature change or a missed expected append invalidates the generation and requires a fresh probe.
  Separately, both 6-second app-callback intervals sampled `PerformanceMetrics` at the FFX callback and the deep
  Present boundary, matching the two ~288-FPS CSV blocks and alternating tiny/normal frame times. Deep Present is now
  the sole timing observer when installed; the callback remains the fallback only when runtime-owned presentation
  does not re-enter CE through DXGI. Draw ownership and frame-timing ownership are deliberately independent.
- **Pacing follow-up (`20260814_041840`)**: displayed cadence was flat between route edges, but CE synchronously
  rebuilt the suspend/topmost renderer inside AMD's live ECL/Present path after callback/no-callback cleanup. The
  test kept FSR enabled but changed that internal route every six seconds; its recurring no-callback entries align
  with 17.3 ms and 16.1 ms ECL stalls and 20.1/48.8 ms displayed-output gaps. Exact presentation identities now
  retain both warm renderer families across routing-only clears, avoiding repeated PSO, 16-slot allocator/list,
  upload-pool, font, and RTV construction. The renderer pins its proxy while the raw-keyed cache is live, drops
  that pin immediately on retirement, and real replacement/context teardown remains authoritative. First-use
  callback/UI behavior is unchanged so the pacing fix cannot trade a stall for a missing fallback draw. The same
  session emitted 3,451 stable UI/deep-site alternation logs and 1,789 identical HDR-source lines; those
  diagnostics are now stateful instead of performing synchronous per-output file I/O.
- **Sources/tests**: `hook/apis/dx12_hook_ffx_topmost_batch.cpp`, `hook/apis/dx12_hook_ffx_metrics.cpp`,
  `hook/common/dx12_overlay_policy/ffx_topmost_batch.h`, `hook/apis/dx12_ffx_suspend_overlay.cpp`,
  and `tests/test_ffx_topmost_batch_policy.cpp`. Focused pacing-policy/source and FSR transition/replay tests pass;
  the complete `--verify` gate also passes on 0.1.6053 (full native/Python suites, lint ratchets, and x64
  ASan/UBSan). **Open**: fresh on-hardware validation of stable translucency/FPS and elimination of CE-attributed
  pacing spikes, plus ReShade validation
  plus the full FSR/off/DLSS switch matrix and teardown/device-health checks after the ownership handoff.

## Open Questions / Stale-Risk
- Stale risk is high because this area depends on call stacks, queue ownership, and third-party module behavior that can change without warning.
- Module-token detection is heuristic. Re-check it whenever new overlay modules appear in traces or bug reports.
- Re-check SL routing suppression whenever FSR FG classification or FFX hook timing changes, because the effective runtime mode is now the authoritative guard.
- The one-time vtable unhook approach (build 0.1.2923) assumes Steam's OverlayHookD3D3 lazily initializes its internal "next" handler on first E9 JMP entry by reading vtable[8], and that the initialization validates the pointer against the real dxgi!Present. This was confirmed by the crash analysis of build 0.1.2922 (which crashed because vtable[8] = DetourPresent failed validation). If Steam's internal initialization mechanism changes (e.g. reads a different vtable slot, uses a non-vtable mechanism, or changes validation criteria), this fix may need revision. The bypass trampoline fallback and `s_steamInitCrashed` flag remain as crash-safe last resorts.
- The vtable[8] fixup + direct Steam handler call approach (build 0.1.2908) is SUPERSEDED by the oPresent routing approach (build 0.1.2922), which is in turn SUPERSEDED by the one-time vtable unhook approach (build 0.1.2923). Neither earlier approach must be restored. The oPresent routing approach incorrectly assumed Steam would accept DetourPresent as a valid forwarding target during initialization; the vtable fixup approach incorrectly assumed Steam reads vtable[8] at call time rather than during E9 JMP initialization.
