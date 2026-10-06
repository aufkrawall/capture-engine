# NGX real-hook lifecycle flow coverage

Last verified: 2026-10-06, clean gate 0.1.7006 and final content-validated verification 0.1.7007.
Both gates passed native units, all 17 FG flows, Python self-tests, x64 ASan/UBSan and lint ratchets.
This is D1 coverage in [architecture-debt-plan.md](../architecture-debt-plan.md).

## Production boundary and controlled runtime

`tests/flow/fakes/ngx/nvngx.cpp` builds as nvngx.dll. It supplies the intercepted D3D12
GetParameters/CreateFeature/EvaluateFeature/ReleaseFeature exports and an independent parameter
vtable model (SetI/UI/F at 3/4/6, GetI/UI at 11/12). Unused slots are never invoked. Controlled
create/evaluate/release failures return real NGX success/failure values. The fake records calls
and resource balance; it makes no hook-routing, FG publication or feature-lifecycle decisions.

`tests/flow/flow_host_ngx.{h,cpp}` loads the core through LoadLibrary and resolves its real exports
after the production hook-thread module pass. The production module/export/parameter hooks and
nvngx_hook_{feature,lifecycle,params,params_fg_factor}.cpp execute normally. The fake deterministically
reuses released feature addresses, so creation/evaluation evidence must belong to the new feature.
Parameter objects and live features are released by the adapter, including assertion-abort paths.
Flow environment teardown independently fails if either live-resource count remains nonzero.
The patched DLL stays loaded until its scenario process exits, like the existing flow hook/runtime
fixtures; unloading while callbacks are admitted remains a separate D7 obligation.

`FlowGame::PublishedNGX` reads the host's actual shared publication. FG active/multiplier come from
one PID-validated publication; SR/RR are separate observations after synchronous export return.
The scenario does not claim that those two independent atomics form a concurrent snapshot.
`PublishedFG` remains the production overlay's cached metrics. Immediate SDK publication is checked
before subsequent polls or rendered frames could overwrite it; visible status is checked after outputs.

## Scenarios and assertions

- `FlowNGX.FeaturePublicationFollowsSuccessfulEvaluationAndRelease`: failed creation/evaluation
  cannot publish activity; successful creation alone does not claim SR/RR rendering. Successful
  evaluations select SR/RR, releasing an unevaluated feature preserves the active one, failed release
  preserves evidence, and releasing the active RR feature falls back to the still-evaluated SR feature.
  Address reuse for a different feature requires new evaluation. Counters prove exports executed;
  repeated cleanup leaves no fake features/parameters. Native outputs retain exactly-once coverage.
- `FlowNGX.InFlightEvaluationCannotOverrideExplicitStreamlineOffAndCanReactivate`: establish a
  native route with one completed output, enable Streamline FG, then evaluate an actual NGX FG handle.
  Accepted OFF followed by an in-flight NGX evaluation must leave PID-owned FG publication off.
  Accepted ON admits NGX reactivation; subsequent output metrics show DLSS 2x. All physical outputs
  are accounted/drawn exactly once and no D3D12 debug-layer errors/corruptions are allowed.

Run through the declared harness:

```powershell
python build.py --incremental --tests-only --flow-tests --run-tests --gtest-filter="FlowNGX.*" --skip-updates --concise
python tools/refactor/check_ngx_flow_mutations.py
```

The focused run `20261006_170651_build_7005` passed both scenarios (5.1 seconds of flow execution).
Its native filter selected zero unit tests: this is flow evidence, not new native-suite coverage.
`check_ngx_flow_mutations.py` temporarily alters only the production lifecycle source, preserves its
exact bytes/newlines, bounds each child build, and restores/retests the original in finally.
It must observe an actual assertion failure in the expected scenario, not a compiler/loader error.
Mutants: ignore accepted OFF at evaluation, omit release republication, and publish creation as rendering.
Full child outputs remain private local artifacts under `build/refactor/ngx-mutations/`.

All three mutants failed the named behavioral scenario and the restored lifecycle passed both cases.
The clean machinery gate `20261006_171431_build_7006` passed, including x64/x86 product builds.
Three touched fixture files had formatting advisories; only reported regions were corrected, with no
whole-file formatter on existing sources. Final `--verify` at `20261006_173138_build_7007` passed
native units, 17 flows (38.3 seconds), Python self-tests, fresh x64 ASan/UBSan and all style/type checks.
The 712 accepted clang-tidy warnings were unchanged; full scope now includes the three new NGX TUs
(991 total). Setup 0.1.7007 is a verified 38,591,852-byte PE. No unit/flow/fuzz processes lingered.
Runtime integration, x86 sanitizers (unavailable), real games, foreign overlays and A/V remain separate.


## Separate cold-start finding and limits

The first attempted OFF/ON scenario enabled DLSS-G immediately after swapchain creation, before
the first native output. It failed physical accounting: 2,432 actual outputs versus 2,410 ledger entries,
with no debug-layer errors; this is not proof that all missing entries were visibly uncovered.
It also checked cached visible metrics immediately after ON, which remained 1x until outputs updated
the cache. The accepted-state check now uses the coherent shared publication instead.

Cold FG startup is distinct from an OFF/ON handover from an established native route. The handover
fixture now proves that precondition with one completed output and a coverage assertion; there is
no sleep, polling delay or production mitigation. **Cold-start accounting remains unfixed** and belongs
to D1/D3 startup route/admission work. Reproduce it by removing the single initial RenderFrame and
its coverage assertion in the second scenario, rebuilding that scenario, and checking the ledger
against physical outputs before/after each interval. Restore the fixture bytes afterward.
Original failed evidence: `20261006_170308_build_7005`, scenario log digest started before inspection.
Source anchors: dx12_hook_postsl_render_entry.cpp synthetic startup admission and cooldown,
dx12_hook_ecl_startup_activation.cpp, and dxgi_shared_present_core.cpp physical-present scopes.
These are investigation anchors, not an established root cause or authorization to shorten a cooldown.

The fake does not run NVIDIA's algorithms, driver scheduling, real GPU workload or actual game clocks.
No real NGX/foreign-overlay compatibility, visual cold-start coverage, hardware performance or A/V
property is established here. Init/reload/unload, callback across retirement, D3D11/Vulkan NGX,
foreign Present installation/removal orders and distinct queue vtables remain D1/D7 follow-ups.
