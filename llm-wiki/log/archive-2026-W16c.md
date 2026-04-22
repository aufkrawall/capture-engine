# llm-wiki Log — Archive 2026-W16c

### 2026-04-16 - Limit post-FSR normal-route bypass transport to comeback families that still have a real stale third-party Present-hook risk

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260416_033646` on build `0.1.2316` showed that the earlier callback-on-normal-route tightening was real but still not the final seam. The late `FSR_FG -> off -> DLSS_FG` comeback still preserved the fresh post-FSR `scQueue=0000024F705E4380`, logged `DX12: Streamline FG ON after FSR — preserving freshly handed-off Streamline swapchain queue ...`, then `Streamline Hook: FG state transition OFF->ON via SetOptions`, then `DetourPresent: Post-FSR startup normal-route bypass #1`. The mirrored `sl-sha-11cf43f.dmp` storm still started immediately afterward. Crucially, there is still no `DX12: PostSL synthetic startup waiting for safe bootstrap path after FSR phase ...` line and no `DX12: PostSL REACTIVATED` line before the dump storm, while the later ECL hook still logs `safeBootstrap=0`. That means the previous callback-entry gate is holding; the remaining issue is not early PostSL callback entry.

- **Root cause refinement**: The earlier post-FSR bypass transport split had become too broad. It was introduced to dodge the stale Steam `gameoverlayrenderer64` Present-hook chain on Talos/Talos-like post-FSR comeback families, but on this GTA session the same unconditional bypass transport was still being applied on the protected post-FSR normal-route family even though the logs no longer showed the older callback-entry mismatch and did not provide evidence that a stale Steam-style Present-hook chain was the thing that needed dodging on this comeback. In other words: route-versus-transport was still the right abstraction, but transport was now being forced onto bypass even when the specific stale third-party hook risk had not been re-established.

- **Fix**:
  1. `hook/common/dxgi_shared.h` now widens the three post-FSR transport helpers with an explicit `staleThirdPartyPresentHookRisk` input:
     `ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(...)`,
     `ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(...)`, and
     `ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(...)`.
  2. `hook/common/dxgi_shared.cpp` now computes that signal from the existing Steam DX12 bypass detection path (`ShouldForceSteamDX12Bypass(...)`) for both `DetourPresent` and `DetourPresent1`.
  3. Post-FSR normal-route transport now stays on bypass only when that stale Steam-style Present-hook risk is actually present. Otherwise CE keeps the logical post-FSR normal-route behavior without forcing unconditional bypass transport.
  4. `tests/test_dxgi_shared.cpp` now updates the three focused transport tests to require the new risk signal, and also corrects the stale callback-gate expectation in `ConfirmedStartupSettlingCanStillInvokePostSLWithoutSyntheticBypass`: safe bootstrap alone is still not enough to unlock post-FSR callback-on-normal-route without explicit `SetOptions` activation.

- **Why this is generic**: This is not another GTA-specific exception. The shared rule remains: post-FSR comeback routing and transport are separate decisions, but transport should only be forced onto bypass when the specific stale third-party Present-hook risk is actually present. Talos proved that risk exists on some post-FSR comeback families; `20260416_033646` proved it should not be assumed universally for every protected post-FSR normal-route Present.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260416_033646/{session_manifest.txt,hook_debug.log,external_sl-sha-11cf43f.dmp}`.
  - Confirmed the decisive failing edge: preserved fresh post-FSR `scQueue=0000024F705E4380`, explicit `OFF->ON via SetOptions`, `Post-FSR startup normal-route bypass #1`, then immediate mirrored Streamline dump storm and later `safeBootstrap=0` with no PostSL activation logs.
  - Confirmed the previous callback gate still held in this run: there is no `PostSL synthetic startup waiting for safe bootstrap path ...` or `PostSL REACTIVATED` line before the dump storm.
  - Re-ran `python build.py --incremental --skip-updates --run-tests`; build succeeded and all 581 tests passed.

- **Files changed**: `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/log.md`

- **Stale risk**: Fresh runtime validation is still required. The next check is whether limiting post-FSR normal-route bypass transport to actual stale third-party Present-hook risk closes the GTA `FSR_FG -> off -> DLSS_FG` seam without reopening the older Talos/Steam crash family that originally justified the bypass transport split.

### 2026-04-16 - Do not invoke PostSL on the protected post-FSR startup normal-route path until the bootstrap topology is actually safe

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260416_031434` on build `0.1.2315` showed that the earlier post-FSR startup-handoff bypass transport split fixed the previous seam but still left one narrower callback-entry bug. The session survives earlier pure-DLSS runs and several mixed switches, then later hits a final `FSR_FG -> STREAMLINE_NO_FG -> DLSS_FG` comeback. CE preserves the fresh post-FSR `scQueue=0000029BED4111C0`, logs `DX12: Streamline FG ON after FSR — preserving freshly handed-off Streamline swapchain queue ...`, then `Streamline Hook: FG state transition OFF->ON via SetOptions`, then `DX12: PostSL synthetic startup takeover — ProcessFrame dormant for 203ms`, then `DetourPresent: Post-FSR startup normal-route bypass #1`. Immediately after that first protected startup callback, Streamline starts emitting `sl-sha-11cf43f.dmp` repeatedly. There is still no `DX12: PostSL synthetic startup waiting for safe bootstrap path after FSR phase ...` line and no `DX12: PostSL REACTIVATED` line for the failing comeback. The later ECL diagnostic in the same failure still says `safeBootstrap=0`.

- **Root cause refinement**: The remaining mismatch was no longer the startup Present transport. `DetourPresent` correctly kept the post-FSR startup Present on the protected normal route and returned it through bypass transport. But the shared callback-on-normal-route helper still let explicit `SetOptions(ON)` unlock PostSL callback entry for that post-FSR startup family even while the bootstrap topology was still unsafe. In other words, explicit comeback authority and safe bootstrap topology were still being conflated one step too early at callback entry.

- **Fix**:
  1. `hook/common/dxgi_shared.h` now tightens `ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(...)` for post-FSR startup.
  2. The post-FSR startup family still needs `explicitSetOptionsActivation`, but it now also requires `safePostFSRBootstrapPath` before callback-on-normal-route can enter PostSL.
  3. The protected startup Present itself is unchanged: it still stays on the normal Streamline route logically and still uses bypass transport.
  4. `tests/test_dxgi_shared.cpp` updates `ConfirmedStartupSettlingCanStillInvokePostSLWithoutSyntheticBypass` to lock the distinction: explicit `SetOptions` alone is no longer enough for post-FSR callback-on-normal-route while startup is still half-armed.

- **Why this is generic**: This is not another GTA-specific branch. The generic rule is that post-FSR comeback authority and post-FSR bootstrap safety are separate questions. A real `OFF->ON via SetOptions` edge says the comeback is authoritative enough to keep the startup Present on the protected normal-route family. It does not by itself prove that the callback may already enter PostSL on that recovered queue topology.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260416_031434/{session_manifest.txt,hook_debug.log,external_sl-sha-11cf43f.dmp}`.
  - Confirmed the healthy earlier parts of the session: initial pure-DLSS startup reaches sustained `Post-SL overlay SUBMIT` traffic, and several earlier OFF/ON cycles remain stable.
  - Confirmed the late failing family: authoritative FSR takeover, long non-FG recovery on the FSR-owned queue, fresh post-FSR Streamline handoff, explicit `OFF->ON via SetOptions`, `Post-FSR startup normal-route bypass #1`, then immediate external dump storm with later `safeBootstrap=0` and no PostSL activation logs.
  - Ran `python build.py --incremental --skip-updates --run-tests`; build succeeded and all 581 tests passed.

- **Files changed**: `hook/common/dxgi_shared.h`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/log.md`, `llm-wiki/frame-generation-switching.md`

- **Stale risk**: Fresh runtime validation is still required. The next check is whether keeping the post-FSR startup Present protected while blocking callback-on-normal-route until `safeBootstrap=1` closes the GTA `FSR_FG -> off -> DLSS_FG` seam without regressing the already working post-FSR repeated-callback stabilization path once wrapper/direct bootstrap evidence appears.

### 2026-04-16 - Keep the first post-FSR startup-handoff Present on the normal route logically, but bypass its transport too

- **Motivation**: Talos `installed/captureengine/logs/20260416_015012` on build `0.1.2313` showed the previous queue-reuse fix was real but still one seam short. The earlier `DLSS_FG -> FSR_FG -> DLSS_FG` resume still worked and reused `lastWorkingQ=000001C0224DFFF0`, but a later fresh `FSR_FG -> off -> DLSS_FG` comeback in the same session crashed before PostSL reactivated. The decisive trace was earlier: CE preserved the fresh post-FSR `scQueue=000001C106E06A70`, logged `DetourPresent: Keeping Streamline startup-handoff Present on the normal SL route #2`, published `runtime=DLSS_FG`, and then the very next artifact was the mirrored external Streamline dump. There were no `PostSL REACTIVATED`, wrapper-progress, probe, or submit logs for that comeback.

- **Root cause refinement**: The surviving seam is the first post-FSR startup-handoff Present itself. The routing layer already knew to keep the startup-handoff Present logically on the normal Streamline route, but `DetourPresent` and `DetourPresent1` still fell through the recovered swapchain's ordinary Present transport, which was too trusting for the fresh third-party hook chain on that post-FSR comeback.

- **Fix**:
  1. `hook/common/dxgi_shared.h` now adds `ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(...)`.
  2. `hook/common/dxgi_shared.cpp` now applies that helper in both `DetourPresent` and `DetourPresent1`, so the first post-FSR startup-handoff Present stays logically on the normal SL route but still returns through the bypass trampoline.
  3. New diagnostics `DetourPresent: Post-FSR startup-handoff normal-route bypass ...` and `DetourPresent1: Post-FSR startup-handoff normal-route bypass ...` make the split visible.
  4. `tests/test_dxgi_shared.cpp` adds `PostFSRStartupHandoffNormalRouteUsesBypassTransport`.

- **Why this is generic**: The shared rule is still routing versus transport. A post-FSR comeback can already have enough evidence to advance startup state on the normal route, while the recovered swapchain's third-party Present hook chain is still unsafe to trust at the transport layer.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260416_015012/{session_manifest.txt,hook_debug.log,crash.log,crash_20260416_015231_609_pid13760_tid8000.dmp,external_sl-sha-bbeb8b77.dmp,external_UEMinidump.dmp}`.
  - Confirmed the earlier `DLSS_FG -> FSR_FG -> DLSS_FG` resume still worked on reused `lastWorkingQueue`, while the later fresh `FSR_FG -> off -> DLSS_FG` comeback crashed before PostSL reactivation.
  - Confirmed the boundary immediately before the crash: preserved fresh post-FSR `scQueue`, normal-route startup-handoff Present, then the mirrored external dump with no PostSL activation/probe/submit logs.
  - Ran `python build.py --incremental --skip-updates --run-tests`; build succeeded and all 581 tests passed.

- **Files changed**: `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/log.md`, `llm-wiki/frame-generation-switching.md`

- **Stale risk**: Fresh Talos runtime validation is still required. The next check is whether the new startup-handoff transport bypass closes the last mixed `FSR_FG -> off -> DLSS_FG` seam without regressing the already working resumed-DLSS queue-reuse path.

### 2026-04-16 - Reuse the validated last-working PostSL queue when a later DLSS-only resume still happens inside stale post-FSR inactive recovery

- **Motivation**: Talos `installed/captureengine/logs/20260416_013624` on build `0.1.2312` disproved the assumption that the next seam was only another post-FSR Present-hook transport issue. The earlier `DLSS_FG -> FSR_FG -> DLSS_FG` comeback in the same run now worked without a crash. The later sequence `DLSS_FG -> all FG off -> DLSS_FG` still crashed, but the trace showed a different family. During the FG-off window, CE kept logging `DX12: Streamline FG OFF after FSR history ...`, `Frame classification using primary queue ... during post-FSR non-FG recovery`, `ProcessFrame queue=... path=lastWorking(post-FSR)`, and offscreen `Post-FSR DLSS overlay via 2-copy compositing` on the preserved `lastWorkingQ=000002BF3A5038E0`. When DLSS resumed, CE still logged `DX12: PostSL REACTIVATED (epoch=4 hadFSR=1 origGame=000002BF6BE2E0D0)`, `DetourPresent: Post-FSR startup normal-route bypass #100`, then locked PostSL to `queue=000002BF6BE2E0D0` with `scQueue=0000000000000000`, submitted `Post-SL overlay SUBMIT #5438`, and immediately logged `DX12: DEVICE_REMOVED detected after PostSL ECL submit #5438 ... hr=0x887A002B` before UE raised the fatal exception.

- **Root cause refinement**: The stale state was not in the later resume's activation edge itself; it was in the recovery classification that survived the clean FG-off interval. `g_HadFSRFGPhase` intentionally stays latched across the session, and the post-FSR inactive-recovery path intentionally leaves `g_SwapchainQueue` unset while non-FG overlay rendering continues on the already validated `g_PostSLLastWorkingQueue`. In this Talos run, that recovery had not yet ended when the later DLSS-only resume occurred. PostSL queue selection in `hook/apis/dx12_hook.cpp` still treated the new ON edge as a fresh post-FSR bootstrap because `hadFSR=1` and `scQueue=null`, so it skipped the validated `lastWorkingQueue` entirely and fell back to the older post-FSR queue-selection family (`origGame` / bootstrap heuristics). That threw away the only queue that had already proved it could render the recovered non-FG topology and reopened a post-FSR `DEVICE_REMOVED` seam on the very first resumed submit.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` now adds `ShouldReuseValidatedPostSLLastWorkingQueueForStreamlineResumeDuringPostFSRInactiveRecovery(...)`.
  2. `hook/apis/dx12_hook.cpp` now computes `explicitSetOptionsActivation` once in PostSL queue selection, and before the normal post-FSR bootstrap fallbacks it checks whether the session is still in stale post-FSR inactive recovery (`hadFSR=1`, `scQueue=null`) but already has stronger resume proof (`lastWorkingQueue` exists plus either explicit `SetOptions` activation or a safe post-FSR bootstrap topology). In that narrow case, PostSL now reuses `g_PostSLLastWorkingQueue` directly instead of discarding it and falling back to the riskier post-FSR queue-selection family.
  3. New diagnostic `DX12: PostSL queue — reusing validated lastWorking queue ... for resumed DLSS activation during post-FSR inactive recovery` makes this path visible in future traces.
  4. `tests/test_dxgi_shared.cpp` adds `ReusesValidatedLastWorkingQueueForResumedDLSSDuringPostFSRInactiveRecovery`.

- **Why this is generic**: This is not a Talos-specific queue hardcode. The generic rule is that a session-level `hadFSR` latch does not mean every later DLSS activation must throw away the currently validated queue proof and re-bootstrap from scratch. If post-FSR inactive recovery is still intentionally active and `scQueue` remains unset, but CE already has a validated `lastWorkingQueue` for the live topology plus a strong new DLSS activation proof, that validated queue is the safest restart point.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260416_013624/{session_manifest.txt,hook_debug.log,crash.log,crash_20260416_013756_372_pid12980_tid18196.dmp,external_UEMinidump.dmp}`.
  - Confirmed this is a different seam than `20260416_012730`: the earlier `DLSS_FG -> FSR_FG -> DLSS_FG` family in the same session worked; the later crash happened only after `DLSS_FG -> all FG off -> DLSS_FG`.
  - Confirmed the stale classification window: long-running `post-FSR non-FG recovery` logs, `scQueue=null`, non-FG overlay rendering on preserved `lastWorkingQ`, then later resumed DLSS activation still reporting `hadFSR=1`.
  - Confirmed the fatal boundary: resumed PostSL locked to `origGame` with `scQueue=null`, passed the post-FSR probes, then immediately hit `DXGI_ERROR_DEVICE_REMOVED / 0x887A002B` after `Post-SL overlay SUBMIT #5438`.
  - Analyzed the dump with `cdb.exe`; crash endpoint is a UE-raised exception after the `DEVICE_REMOVED` submit, not the older `gameoverlayrenderer64!OverlayHookD3D3 -> sl_dlss_g` null-call family.
  - Ran `python build.py --incremental --skip-updates --run-tests`; build succeeded and all 580 tests passed.

- **Files changed**: `hook/common/dx12_overlay_policy.h`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/log.md`, `llm-wiki/frame-generation-switching.md`
- **Stale risk**: Fresh runtime validation is still required. If Talos still crashes after this queue-reuse fix, the next seam is likely in when post-FSR inactive recovery itself should end, not in the later resumed PostSL queue bootstrap path.

### 2026-04-16 - Keep post-FSR confirmed standalone Streamline Presents on the normal route logically, but still use bypass transport

- **Motivation**: Talos `installed/captureengine/logs/20260416_012730` on build `0.1.2311` proved the previous expiry-triggered ECL gate fix worked, but exposed the next seam one step later. The post-FSR comeback now stayed dormant correctly while unsafe: the log repeats `DX12: PostSL synthetic startup waiting for safe bootstrap path after FSR phase (realQ=0 realECL=00007FFE5C949470 slWrapper=0)` and then explicitly logs `DX12: ECL hook leaving pending PostSL activation dormant after startup window expiry because post-FSR bootstrap path is still unsafe ... safeBootstrap=0`. Later, once wrapper-derived progress existed, the visible-overlay ECL path reactivated PostSL, `DetourPresent` kept the half-armed family on `Post-FSR startup normal-route bypass #1..#10`, CE rebuilt overlay state on preserved `scQueue=000002536AA3A820`, passed post-FSR probes, confirmed rendering, and submitted `Post-SL overlay SUBMIT #2368..#2376`. The crash still immediately returned in the same family: `0x0 -> gameoverlayrenderer64!OverlayHookD3D3 -> capture_hook_x64 -> sl_dlss_g`.

- **Root cause refinement**: The surviving seam is no longer the half-armed startup family. After confirmation, the recovered post-FSR path leaves the earlier startup-bypass branch and falls into the later `confirmed standalone Streamline Present on the normal SL route` path in `hook/common/dxgi_shared.cpp`. That branch correctly invokes PostSL so visible rendering continues, but it still falls through the normal `CallOriginalPresent()` / `CallOriginalPresent1()` transport path instead of using the bypass trampoline. The Talos trace shows this later boundary clearly: there are no more `Post-FSR startup normal-route bypass ...` lines once confirmed standalone Presents take over, yet the very next crash is still the stale-Steam-hook null-call family. So routing was already correct; transport was still too trusting on the recovered swapchain.

- **Fix**:
  1. `hook/common/dxgi_shared.h` now adds `ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(...)`.
  2. `hook/common/dxgi_shared.cpp` now applies that helper in both `DetourPresent` and `DetourPresent1`: on DX12 post-FSR comebacks, when a confirmed standalone Streamline Present is kept on the normal route and used to invoke PostSL, transport also returns through `EnsurePresentBypassTrampoline()` / `EnsurePresent1BypassTrampoline()`.
  3. New diagnostics `DetourPresent: Post-FSR confirmed standalone normal-route bypass ...` and `DetourPresent1: Post-FSR confirmed standalone normal-route bypass ...` make the new split visible.
  4. `tests/test_dxgi_shared.cpp` adds `PostFSRConfirmedStandaloneNormalRouteUsesBypassTransport` to lock the invariant in place.

- **Why this is generic**: This is not a Talos-specific Steam workaround. The generic rule is the same routing/transport separation used earlier for half-armed startup Presents: a post-FSR comeback can already have enough shared-state evidence to keep later standalone Streamline Presents on the normal SL route and to invoke PostSL there, while the recovered swapchain's third-party Present hook chain is still not safe to trust. Routing stays topology-driven; transport remains on bypass.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260416_012730/{session_manifest.txt,hook_debug.log,crash.log,crash_20260416_012838_015_pid14144_tid15476.dmp,external_UEMinidump.dmp}`.
  - Confirmed the previous fix held: expiry-time ECL activation stayed blocked while `safeBootstrap=0`.
  - Confirmed the new seam: later visible-overlay ECL progress reactivated PostSL, startup normal-route bypass covered only the half-armed phase, CE then confirmed rendering and submitted `#2368..#2376` before the crash returned.
  - Analyzed the dump with `cdb.exe`; stack remains `0x0 -> gameoverlayrenderer64!OverlayHookD3D3 -> capture_hook_x64 -> sl_dlss_g`.
  - Ran `python build.py --incremental --skip-updates --run-tests`; build succeeded and all 579 tests passed.

- **Files changed**: `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/log.md`, `llm-wiki/frame-generation-switching.md`
- **Stale risk**: Fresh Talos runtime validation is still required. If another crash remains after this split, the next seam will likely be in what `CallOriginalPresent()` trusts after the confirmed-standalone bypass path, not in the earlier startup routing state machine.

### 2026-04-16 - Block expiry-triggered ECL startup activation on post-FSR comebacks until the safe bootstrap topology exists

- **Motivation**: Talos `installed/captureengine/logs/20260416_011843` on build `0.1.2310` still crashed at the end of `DLSS_FG -> FSR_FG -> DLSS_FG`, but the trace exposed a more precise contradiction than the previous run. CE preserved the fresh post-FSR Streamline `scQueue=000001C4685D8030`, correctly kept logging `DX12: PostSL synthetic startup waiting for safe bootstrap path after FSR phase (realQ=0 realECL=00007FFE5C949470 slWrapper=0)`, then the ECL hook still logged `startup transition window expiry with pending PostSL activation — triggering PostSL callback directly from ECL context to complete activation`. Seconds later CE reactivated PostSL, used `DetourPresent: Post-FSR startup normal-route bypass #1..#10`, rebuilt overlay state on the preserved `scQueue`, passed post-FSR probes, confirmed rendering, submitted `#2295..#2303`, and then crashed again in the same Steam / `sl_dlss_g` null-call family.

- **Root cause refinement**: The earlier safe-bootstrap gating fix only covered the periodic visible-overlay ECL startup-progress path. A second ECL path, the one-shot `startupTransitionWindowJustExpired` direct callback trigger in `hook/apis/dx12_hook.cpp`, still ignored that policy entirely. In this Talos trace the contradiction is explicit: the same run logs `waiting for safe bootstrap path after FSR phase` and then immediately activates PostSL anyway from the expiry callback even though the bootstrap inputs are still unsafe (`realQ=0`, `slWrapper=0`). That let PostSL advance into the later post-FSR normal-route/bypass family from an activation source that had skipped the intended topology gate.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` now adds `ShouldTriggerExpiryDrivenECLPostSLStartupActivation(...)` so the expiry-time ECL trigger and the periodic ECL startup-progress path share the same post-FSR safety rule.
  2. `hook/apis/dx12_hook.cpp` now computes `safePostFSRBootstrapPath` once, threads it into both ECL startup-progress decisions, and blocks the expiry-triggered direct callback while `hadFSR=1` and the safe bootstrap topology is still unavailable.
  3. The ECL hook now logs `leaving pending PostSL activation dormant after startup window expiry because post-FSR bootstrap path is still unsafe ...` when it deliberately preserves the half-armed state instead of forcing activation.
  4. `tests/test_dxgi_shared.cpp` adds `ExpiryDrivenECLStartupActivationRespectsPostFSRSafeBootstrapGate` to lock the new invariant in place.

- **Why this is generic**: This is not another Talos-specific branch. The generic rule is that all ECL-side startup-activation paths must obey the same post-FSR topology safety boundary. If `PostSLOverlayRender()` itself would still delay activation because the comeback lacks a safe bootstrap path, the ECL expiry callback must not be allowed to jump around that gate.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260416_011843/{session_manifest.txt,hook_debug.log,crash.log,crash_20260416_011934_664_pid9240_tid13284.dmp,external_UEMinidump.dmp}`.
  - Confirmed the contradiction in the trace: repeated `waiting for safe bootstrap path after FSR phase`, then `ECL hook detected startup transition window expiry ... triggering PostSL callback directly`, later `Post-FSR startup normal-route bypass #1..#10`, overlay bootstrap on preserved `scQueue`, post-FSR probe success, confirmation, and `Post-SL overlay SUBMIT #2295..#2303` before the crash.
  - Analyzed the dump with `cdb.exe`; the crash stack remains `0x0 -> gameoverlayrenderer64!OverlayHookD3D3 -> capture_hook_x64 -> sl_dlss_g`, so this patch specifically closes an activation-path contradiction rather than claiming the full Talos family is solved.
  - Ran `python build.py --incremental --skip-updates --run-tests`; build succeeded and all 578 tests passed.

- **Files changed**: `hook/common/dx12_overlay_policy.h`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/log.md`, `llm-wiki/frame-generation-switching.md`
- **Stale risk**: Fresh Talos runtime validation is still required. If a later crash remains after this gating fix, the next seam will be after a genuinely safe post-FSR activation path rather than this earlier ECL-expiry bypass around the topology gate.

### 2026-04-16 - Keep post-FSR normal-route Present transport on bypass through the confirmed-startup-settling window

- **Motivation**: Talos `installed/captureengine/logs/20260416_011004` on build `0.1.2309` still crashed at the end of the `DLSS_FG -> FSR_FG -> DLSS_FG` switching sequence. The previous fixes were working up to a later boundary than before: CE preserved the fresh post-FSR Streamline `scQueue=0000020AF4B01300`, kept decisive startup Presents on the normal route while logging `DetourPresent: Post-FSR startup normal-route bypass #1..#10`, rebuilt torn-down overlay state after warm-up, passed the post-FSR level-0 probes on the selected `scQueue`, confirmed rendering, and logged `Post-SL overlay SUBMIT #2085` and `#2086`. The crash still immediately returned in the old family: `0x0 -> gameoverlayrenderer64!OverlayHookD3D3 -> capture_hook_x64 -> sl_dlss_g`.

- **Root cause refinement**: The remaining seam was still in shared DXGI transport, but one step later than the previous fix. `ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(...)` dropped the bypass as soon as `postSLConfirmedRendering` became true. Talos proves that the first successful post-FSR render is still not enough to trust Steam's fresh-swapchain DX12 Present hook chain again. During the short confirmed-but-startup-settling window, Streamline-originated Presents still belong to the fragile startup family; falling through normal `oPresent` / `oPresent1` there can re-enter `gameoverlayrenderer64` before Steam's saved original Present pointer on the recovered swapchain is stable.

- **Fix**:
  1. `hook/common/dxgi_shared.h` now widens `ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(...)` with one more state input: `postSLConfirmedButStartupSettling`.
  2. On DX12 post-FSR comebacks, bypass transport now remains active until PostSL has both confirmed rendering and left that short startup-settling window.
  3. `hook/common/dxgi_shared.cpp` now threads that settling signal through both `DetourPresent` and `DetourPresent1`, and the diagnostics now log `settling=` on the post-FSR normal-route bypass path.
  4. `tests/test_dxgi_shared.cpp` renames and extends the focused regression test to `PostFSRStartupNormalRouteUsesBypassUntilPostSLSettles`.

- **Why this is generic**: This is not a Talos-specific branch and not a Steam-specific quirk hardcoded into policy. The shared rule is that PostSL's first successful render proves CE's recovered overlay path, but it does not prove that a third-party overlay's fresh-swapchain Present hook chain is already safe to trust again. On post-FSR DX12 comebacks, routing can remain topology-driven while actual Present transport stays on the bypass trampoline through the short confirmed-startup-settling window.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260416_011004/{session_manifest.txt,hook_debug.log,crash.log,crash_20260416_011049_873_pid24748_tid25984.dmp,external_UEMinidump.dmp}`.
  - Confirmed the pre-crash sequence: preserved fresh post-FSR `scQueue`, repeated `Post-FSR startup normal-route bypass ...`, warm-up completion, overlay bootstrap rebuild on `scQueue=0000020AF4B01300`, passed post-FSR probes, then `DX12: PostSL CONFIRMED rendering via re-entrant Present` plus `Post-SL overlay SUBMIT #2085/#2086` immediately before the crash.
  - Analyzed the primary dump with `cdb.exe`; the crash stack is again `0x0 -> gameoverlayrenderer64!OverlayHookD3D3 -> capture_hook_x64 -> sl_dlss_g`, confirming a return to the stale-Steam-hook family after first confirmation.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 577 tests passed.

- **Files changed**: `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/log.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/dx12-overlay-third-party-coexistence.md`
- **Stale risk**: Fresh Talos runtime validation is still required. If a later crash remains after this settling-window transport clamp, the next seam will likely be after startup settling has ended rather than in the earlier half-armed comeback transport path.

### 2026-04-16 - Stop post-FSR wrapper bootstrap once the comeback already has a preserved runtime-owned swapchain queue and a safe bootstrap topology

- **Motivation**: Talos `installed/captureengine/logs/20260416_010150` on build `0.1.2308` proved the previous Steam-hook transport fix worked, but exposed the next seam immediately afterward. The old `0x0 -> gameoverlayrenderer64!OverlayHookD3D3` crash family is gone in this run. CE now keeps the comeback on the normal Streamline route, logs repeated `DetourPresent: Post-FSR startup normal-route bypass ...`, survives warm-up, rebuilds overlay state, and reaches the first real post-FSR queue/probe stage. The crash then changes to a `KERNELBASE!RaiseException` / UE crash path after `DX12: PostSL post-FSR PROBE level=0 (scratch-barrier) ... devRemoved=0x887A0001 FAILED`.

- **Root cause refinement**: The remaining failure was no longer about Present routing. After warm-up, `hook_debug.log` shows CE still choosing `DX12: PostSL queue — wrapper bootstrap 0000011E222842A0 (validatedCmdQ=00000120EE1FF1F0 latestWrapper=0000011E222842A0 scQueue=00000120EE200FF0 hadFSR=1)` even though the comeback already had both stronger recovery signals: a preserved runtime-owned Streamline swapchain queue (`scQueue=00000120EE200FF0`) and a safe post-FSR bootstrap topology strong enough to leave the synthetic/bypass family. Bootstrapping through the wrapper at that point re-opened the older post-FSR `DEVICE_REMOVED` seam on the very first level-0 probe. This is the same family the older GTA `0.1.2296` explicit-enable runs exposed, but Talos now reaches it only after the newer Present-transport fixes removed the earlier crash boundaries.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` now widens `ShouldUsePostSLWrapperBootstrapQueueAfterFSR(...)` with two more state inputs: whether a preserved runtime-owned swapchain queue already exists, and whether the comeback already has a safe post-FSR bootstrap topology.
  2. Once both are true, wrapper bootstrap is now blocked even if an SL wrapper queue is available and the comeback still lacks an explicit `OFF->ON via SetOptions` activation edge. The stronger topology evidence now wins over the older wrapper-bootstrap fallback.
  3. `hook/apis/dx12_hook.cpp` now threads `hasRuntimeOwnedSwapchainQueue` and `HookHasSafePostFSRBootstrapPath()` into that policy call when selecting the initial post-FSR PostSL queue.
  4. `tests/test_dxgi_shared.cpp` extends `PostSLUsesWrapperBootstrapQueueAfterFSROnlyWhenDirectPathIsUnavailable` so future changes cannot regress this boundary.

- **Why this is generic**: This is not a Talos-specific special case. The shared post-FSR queue-selection policy already distinguished weak fallback evidence (wrapper bootstrap) from stronger evidence (validated direct path, preserved swapchain queue, safe bootstrap topology). The bug was that wrapper bootstrap still stayed enabled even after the stronger topology had already been proven good enough to leave the startup routing guard. The generic rule is now consistent: once CE already has a preserved runtime-owned `scQueue` and a safe post-FSR bootstrap topology, it must not route back through wrapper bootstrap just because the direct queue-behind-wrapper capture has not materialized yet.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260416_010150/{session_manifest.txt,hook_debug.log,crash.log,crash_20260416_010240_545_pid2916_tid5036.dmp,external_UEMinidump.dmp}`.
  - Confirmed the old Steam-null-pointer seam is gone in this run: the log repeatedly shows `DetourPresent: Post-FSR startup normal-route bypass ...` and no `RIP=0x0` / `gameoverlayrenderer64` crash path returns.
  - Confirmed the new failure boundary: after warm-up and overlay bootstrap, CE still selected `wrapper bootstrap ... scQueue=00000120EE200FF0 hadFSR=1`, then immediately logged `DX12: PostSL post-FSR PROBE level=0 (scratch-barrier) ... devRemoved=0x887A0001 FAILED` before the UE-side `RaiseException` crash.
  - Analyzed the primary dump with `cdb.exe`; this crash is no longer the old Steam hook family and instead surfaces as a UE-raised failure path after the `DEVICE_REMOVED` probe.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 577 tests passed and the build completed successfully.

- **Files changed**: `hook/common/dx12_overlay_policy.h`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/log.md`, `llm-wiki/frame-generation-switching.md`
- **Stale risk**: Fresh Talos runtime validation is still required. The new rule blocks wrapper bootstrap once a safe post-FSR topology and preserved runtime-owned `scQueue` already exist, but the next remaining seam - if any - will likely be in the fallback path chosen after wrapper bootstrap is denied.

### 2026-04-16 - Keep post-FSR startup Presents on the normal Streamline route logically, but bypass stale third-party Present hooks until PostSL confirms

- **Motivation**: Talos `installed/captureengine/logs/20260416_003131` on build `0.1.2307` still crashed at the end of the `DLSS_FG -> FSR_FG -> DLSS_FG` switching sequence with `0xC0000005` at `RIP=0x0`. The new trace was tighter than the earlier unsafe-bootstrap failure: CE now waited for safe topology, then later reactivated PostSL through the ECL-side visible-overlay startup progress path, but the very next comeback Present still crashed in the same family: `gameoverlayrenderer64!OverlayHookD3D3 -> capture_hook_x64 -> sl_dlss_g -> sl_interposer`.

- **Root cause refinement**: The post-FSR comeback routing and the actual Present transport path were still conflated. `hook/common/dxgi_shared.cpp` correctly decided that the half-armed post-FSR startup family had to stay on the normal Streamline route so PostSL could keep making progress, and it also correctly invoked the PostSL callback on that normal route. But after that callback ran, the code still fell through to the normal `oPresent` / `oPresent1` path. On the recovered swapchain, Steam's DX12 vtable hook chain had not yet stabilized its saved original Present pointer, so `oPresent` re-entered `gameoverlayrenderer64` and Steam called a null original function pointer. The crash session proves this split directly: after `DX12: PostSL REACTIVATED (epoch=3 hadFSR=1)`, the crash dump stack is `0x0 -> gameoverlayrenderer64!OverlayHookD3D3 -> capture_hook_x64 -> sl_dlss_g`, and there is still no confirmed PostSL render before the fault.

- **Fix**:
  1. `hook/common/dxgi_shared.h` now adds `ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(...)`. It captures the new invariant: on DX12 post-FSR comebacks, if the startup Present must stay in the normal Streamline family but PostSL has not confirmed rendering yet, the actual Present transport must still use the bypass trampoline.
  2. `hook/common/dxgi_shared.cpp` now computes `keepStartupPresentOnNormalRoute` once and uses the new helper in both `DetourPresent` and `DetourPresent1`. CE still invokes the normal-route PostSL callback so startup progress continues, but it now returns through `EnsurePresentBypassTrampoline()` / `EnsurePresent1BypassTrampoline()` until the recovered path confirms at least one successful PostSL render.
  3. The new diagnostics `DetourPresent: Post-FSR startup normal-route bypass ...` and `DetourPresent1: Post-FSR startup normal-route bypass ...` make this routing/transport split visible in future traces.
  4. `tests/test_dxgi_shared.cpp` now adds `PostFSRStartupNormalRouteUsesBypassUntilPostSLConfirms` to lock the new boundary in place.

- **Why this is generic**: This is not a Steam-only special case layered onto Talos. The generic rule is that shared startup-routing policy and the underlying Present transport are different concerns. After an FSR-owned swapchain handoff, a third-party overlay may still have stale hooks on the new swapchain even though CE already has enough shared-state evidence to keep the comeback in the normal Streamline startup family. The callback/routing decision should remain topology-driven, but the actual Present call must stay on the bypass trampoline until the recovered post-FSR path has confirmed a real render.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260416_003131/{session_manifest.txt,hook_debug.log,crash.log,crash_20260416_003231_647_pid19800_tid17856.dmp,external_sl-sha-bbeb8b77.dmp}`.
  - Confirmed the pre-crash sequence: repeated `DX12: PostSL synthetic startup waiting for safe bootstrap path after FSR phase`, ECL-side startup-progress activation, `DX12: PostSL REACTIVATED (epoch=3 hadFSR=1)`, then immediate crash before any confirmed PostSL render.
  - Analyzed the primary dump with `cdb.exe` using the session PDBs; the recovered stack is `0x0 -> gameoverlayrenderer64!OverlayHookD3D3 -> capture_hook_x64 -> sl_common/sl_dlss_g/sl_interposer`, which matches Steam calling a null original Present hook on the new swapchain.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 577 tests passed and the build completed successfully.

- **Files changed**: `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/log.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/dx12-overlay-third-party-coexistence.md`
- **Stale risk**: The new routing split assumes bypass trampolines exist and remain the safest transport while third-party hooks settle on a new post-FSR swapchain. Re-check this area if Present bypass ownership, Steam coexistence rules, or the definition of post-FSR startup confirmation changes.

### 2026-04-16 - Fix `HasExplicitSetOptionsActivationForCurrentComeback()` bypass via steady-state enable + gate ECL-driven startup progress on safe bootstrap topology

- **Motivation**: Talos `installed/captureengine/logs/20260416_001322` on build `0.1.2304` still crashes at the end of the DLSS FG -> FSR FG -> DLSS FG switching sequence with `0xC0000005` at `RIP=0x0` through `gameoverlayrenderer64!OverlayHookD3D3 -> capture_hook_x64 -> sl_dlss_g -> sl_interposer`. The crash is a null function pointer call through the Steam overlay's DX12 hook chain after the FSR-to-DLSS queue transition.

- **Root cause**: Two related issues in the post-FSR comeback trust model:
  1. `HasExplicitSetOptionsActivationForCurrentComeback()` in `streamline_hook.cpp` checked `!g_BlockGetStateOnlyReactivationUntilExplicitSetOptions`, which is cleared by ANY `slDLSSGSetOptions(enable)` call including steady-state enable requests that are NOT fresh OFF->ON activation edges. This broader signal was used by the DXGI routing guards (`ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute`, `ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute`) to unlock the protected post-FSR normal-route family. In the crash session, a later steady-state `slDLSSGSetOptions(enable)` at `00:14:06.697` retroactively satisfied this signal even though the comeback had only ever activated via `GetState` and the bootstrap topology was still unsafe (`realQ=0, slWrapper=0`). This allowed the first comeback Present to invoke PostSL on the normal route while the safe bootstrap check was still blocking activation inside `PostSLOverlayRender()`. The mismatch between the routing-level trust and the activation-level check caused PostSL to be invoked on a queue where the Steam overlay's hook chain had a stale null function pointer from the FSR handoff.
  2. `ShouldContinueECLDrivenPostSLStartupProgress()` in `dx12_overlay_policy.h` did not check for safe bootstrap topology after FSR. This allowed the ECL hook's startup progress continuation path to drive `PostSLOverlayRenderGated()` -> `PostSLOverlayRender()` even when the post-FSR bootstrap topology was still unsafe (`realQ=0, slWrapper=0`), which could advance PostSL's state machine through activation on an unsafe queue path.

- **Fix**:
  1. `HasExplicitSetOptionsActivationForCurrentComeback()` now requires BOTH conditions: the suppression block was cleared AND the current comeback's fresh activation edge was actually via `SetOptions` (not just any later steady-state enable). This closes the gap where `g_BlockGetStateOnlyReactivationUntilExplicitSetOptions` (cleared by any enable) and `g_CurrentComebackActivatedViaExplicitSetOptions` (only set on fresh OFF->ON SetOptions edges) diverged. The routing guards now use the same narrowed meaning that `0.1.2298` intended.
  2. `ShouldContinueECLDrivenPostSLStartupProgress()` now takes `hadFSRFGPhase` and `safePostFSRBootstrapPath` parameters. For post-FSR comebacks without a safe bootstrap topology, it returns false, preventing the ECL hook from driving PostSL into activation/rendering on an unsafe queue path. Non-FSR paths are unaffected (both parameters are `false`).
  3. New test `VisibleOverlayBlocksECLDrivenStartupProgressForPostFSRWithoutSafeBootstrap` locks both new invariants in place.

- **Files changed**: `hook/apis/streamline_hook.cpp`, `hook/common/dx12_overlay_policy.h`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`
- **Wiki pages affected**: `current.md`, `log.md`, `frame-generation-switching.md`
- **Stale risk**: The narrowed `HasExplicitSetOptionsActivationForCurrentComeback()` is now the canonical trust boundary for post-FSR routing. Any future code that checks this signal should be aware it now requires a fresh OFF->ON SetOptions edge, not just any enable request.

