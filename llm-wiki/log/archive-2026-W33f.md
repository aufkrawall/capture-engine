# llm-wiki Log Archive (2026-08-13, shard W33f)

Entries are newest-first. Rotated out of `recent.md` on 2026-08-13 to keep it near
the 230-line rolling-memory ceiling.

### 2026-08-13 - FIXED: ReShade proxy queue re-entry in the ECL/Signal trace hooks (Talos crash)

- Talos (DX12) + ReShade-only crashed on start twice, on both sides of the same layered chain
  `game -> CE -> ReShade proxy thunk -> CE (real queue) -> global original(real queue)`.
- Session `20260813_041416` (build 0.1.5990): the ECL recursion-break path called the global
  `oExecuteCommandLists` (= ReShade's proxy hook, the first queue vtable CE hooked) with the real queue
  behind the proxy; ReShade's non-recursive queue mutex threw `std::system_error(EDEADLK)` (verified via
  the throw-info/catchable-type decode in cdb).
- Session `20260813_050515` (build 0.1.5993): ECL was fixed but the same blind-global pattern remained in
  `DetourTraceCommandQueueSignal` (`oTraceCommandQueueSignal` = ReShade's Signal thunk); calling it with the
  real queue read `_orig` at `queue+0x10` and jumped through garbage vtable slot `-1` (AV at
  `reshade+0x112467`).
- Fix (builds 0.1.5991/0.1.5995): type-safe per-vtable original resolution — policy
  `hook/common/dx12_overlay_policy/ecl_recursion_break.h` classifies candidates by owning module, native
  D3D12 runtime ECL is only used for native-vtable queues, proxy queues only forward through their exact
  vtable original, and foreign/self hooks are never re-entered (recursion-depth bound drops instead of
  looping). Native originals are published eagerly (`TryPublishRealD3D12ECLCandidate` /
  `TryPublishRealD3D12SignalCandidate` from `DX12_HookQueueVTable`); Signal forwards per-vtable
  (`dx12_hook_g_CommandQueueSignalOriginalByVTable`) with live-slot/native/legacy fallbacks.
- Tests: `tests/test_dx12_ecl_recursion_break_policy.cpp` (policy + source pins). Verify gate passed on
  0.1.5995. Needs the user's Talos re-test with all tool combinations.
