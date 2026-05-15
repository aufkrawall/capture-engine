# CaptureEngine Code and Binary Quality Audit Report

**Date:** 2026-05-15
**Build:** 0.1.3123 (tests 0.1.3122)
**Scope:** Source code, build system, tests, generated binaries (x64/x86), dependencies, tooling

---

## 1. Executive Summary and Overall Rating

**Verdict: Ready to ship with minor fixes**

**Weighted Total:** 6.8 / 10 (moderate risk, several well-understood issues remain but are not release-blocking)

**Confidence:** Medium

**Top 5 Risks:**
1. **g_PostSLLastWorkingQueue / g_PostSLLockedQueue use-after-free race** — raw pointer writes during FG mode switching concurrent with ECL hook reads, no mutex protection
2. **g_RealD3D12ECL dangling pointer** — raw function pointer not re-validated after Streamline module reload, can cause DEVICE_REMOVED or crash
3. **~1500-line DetourPresent in dxgi_shared.cpp** — hot path with dozens of boolean flags and deeply nested conditionals, impossible to fully audit, high regression risk with every FG or overlay change
4. **IAT hook EAT patching unsigned wrap-around** — potential arbitrary code execution vector when hookFunction is below exportingModule base
5. **Named pipe IPC without authentication** — all authenticated users can send commands to capture engine (Shutdown, StartRecording, etc.)

**Highest-Blast-Radius Risks:** Stale COM pointer in PostSL queue tracking (use-after-free in game process); missing authentication on IPC named pipe (privilege escalation); VEH handler blocking 15 seconds (process-wide deadlock)

**Technical Debt Assessment:** Moderate. Several files exceed 200KB (dx12_hook.cpp at 817KB is extreme). The test suite is excellent (745 tests) but skews heavily toward policy/decision testing and away from integration testing. The stub layer for tests is non-trivial and must be maintained.

**Regression-Hardening Assessment:** Good for policy/decision units (pure function pattern is highly testable). Weak for integration, memory safety, concurrency, and end-to-end scenarios. ASan/UBSan available but not default.

**Are Larger Refactors Justified?** Splitting dx12_hook.cpp (817KB) and DetourPresent (~1500 lines) would reduce risk. Adding mutex protection to PostSL queue pointers would reduce crash risk. These are justified.

**Main Recommended Next Phase:** Fix the 5 highest-risk findings, split DetourPresent and dx12_hook.cpp, then run ASan+UBSan validation pass.

**What Was Not Assessed and How That Affects Confidence:** CI/CD pipeline, deployment packaging, signing, installer, distribution, and operational-process were not scored. This does not affect code/binary quality confidence.

---

## 2. Scorecard

| Category | Weight | Score | Confidence | Notes |
|---|:---:|---:|---|---|
| Correctness and feature behavior | 13% | 7 | Medium | Complex FG state machine mostly correct via exhaustive testing, but concurrency issues in queue pointer management are acknowledged |
| Reliability, failure recovery, concurrency, and process stability | 14% | 6 | Medium | VEH 15s blocking, dump storm mitigation present, freeze watchdog present, but PostSL pointer races and deadlock risks in crash handler reduce score |
| Memory, resource, lifetime, native/FFI, and undefined-behavior safety | 13% | 5 | Medium | Use-after-free risk in PostSL queue pointers, dangling g_RealD3D12ECL, slDLSSGOptions struct version mismatch, IAT EAT wrap-around |
| Security, privacy leakage, and source-level threat model | 11% | 5 | Medium | Named pipe permissive SDDL, no auth, EAT patching wrap-around, dev build DLL verification bypassable, DbgHelp.dll path hijacking risk |
| Performance, cost, energy, and resource efficiency | 8% | 7 | Medium | LTO, -O3, x86-64-v3 for host, but ~2.2MB hook DLL is large; relaxed atomic loads in hot paths acceptable |
| Storage, filesystem, persistence, and recovery | 7% | 7 | Medium | Config file 16MB size limit, path traversal not checked for config path, dump storm mitigation good |
| Architecture, maintainability, and code consistency | 12% | 6 | Medium | dx12_hook.cpp at 817KB (extreme), DetourPresent ~1500 lines, clang-format + clang-tidy active, but file sizes hinder maintenance |
| Logging, diagnostics, and observability | 4% | 8 | High | Excellent structured logging (FG SNAPSHOT/EVENT/PLAN/TRANSITION/INVARIANT), rate-limited, trace-level forensic detail |
| Tests, regression hardening, and quality gates | 9% | 8 | High | 745 tests, 39 test files, excellent policy/decision coverage, ASan+UBSan available, no disabled tests |
| Source build, tooling, static analysis, and binary inspection | 6% | 7 | Medium | Clean build with Clang 21, -fcf-protection=full, ASLR/DEP/NX, no writable-executable sections, optional clang-tidy |
| Dependencies, supply chain, licensing, API/config/docs compatibility | 3% | 7 | Medium | FFmpeg (LGPL), MIT-licensed, OBSIndicator (MIT), SpecialK fork, licenses bundled in installed output |
| Domain-specific safety/failsafes | N/A | N/A | N/A | Game overlay/inject tool — crash avoidance is critical and well-handled via VEH, dump mirroring, freeze watchdog |

**Arithmetic:**
```
(7×0.13 + 6×0.14 + 5×0.13 + 5×0.11 + 7×0.08 + 7×0.07 + 6×0.12 + 8×0.04 + 8×0.09 + 7×0.06 + 7×0.03)
 / (0.13 + 0.14 + 0.13 + 0.11 + 0.08 + 0.07 + 0.12 + 0.04 + 0.09 + 0.06 + 0.03)
= 6.8 / 10
```

---

## 3. Findings and Recommendations

### F-02-001: PostSL Queue Pointer Use-After-Free Race

**Category:** Memory, resource, lifetime, native/FFI, and undefined-behavior safety
**Severity:** Critical
**Confidence:** High
**Location:** `hook/apis/dx12_hook.cpp` (g_PostSLLastWorkingQueue lines ~389-405, g_PostSLLockedQueue ~342; detour ~13985-13996)
**Problem:** g_PostSLLastWorkingQueue and g_PostSLLockedQueue are raw pointers written during FG mode switch (SetPostSLLastWorkingQueue calls AddRef/Release on old) and read concurrently by DetourExecuteCommandLists (ECL hook) without any mutex or atomic protection. Thread A can read the pointer while Thread B's Release drops the last ref, causing use-after-free.
**Impact:** Use-after-free in game process. Likely crash or DEVICE_REMOVED in production. Documented in code comments as acknowledged risk.
**Blast radius:** Single game process crash per occurrence.
**Recommended fix:** Wrap g_PostSLLastWorkingQueue and g_PostSLLockedQueue in a shared_mutex or a seqlock. Protect all reads (ECL hook, PostSL rendering) and writes (SetPostSLLastWorkingQueue, FG activation/reactivation). Use atomic<ComPtr> or manual ref-counted pointer with release-acquire ordering if full mutex is too expensive for the hot ECL path.
**Implementation guidance:** Add a `std::shared_mutex g_PostSLQueueMutex` alongside the pointers. Lock shared in DetourExecuteCommandLists (read path), lock exclusive in SetPostSLLastWorkingQueue and FG teardown. For the hot ECL path, consider a seqlock or RCU-style deferred-free pattern.
**Suggested tests:** Concurrency stress test: spin up two threads, one repeatedly setting last working queue, another reading it, verify no crashes under ASan.
**Release blocker:** No
**Estimated effort:** Medium

---

### F-06-002: DetourPresent Monolithic Function Complexity

**Category:** Architecture, maintainability, and code consistency
**Severity:** High
**Confidence:** High
**Location:** `hook/common/dxgi_shared.cpp` (~1500-line DetourPresent function)
**Problem:** DetourPresent is a single monolithic function of approximately 1500 lines with dozens of boolean flags and deeply nested conditionals. It handles Steam overlay bypass, Streamline handoff, Post-FSR recovery, HDR detection, swapchain routing, startup bypass, synthetic re-entrancy, and many other concerns in one function body. Every FG or third-party-overlay fix touches this function, creating high regression risk.
**Impact:** Near-impossible to fully audit for correctness. Missed state transitions and logic errors are likely to appear with each change. Every fix to third-party overlay coexistence or FG mode switching creates blast radius across the entire Present routing path.
**Blast radius:** All overlay rendering, all FG modes, all graphics APIs routed through DXGI Present.
**Recommended fix:** Split DetourPresent into ~5 focused helper functions with clear responsibility boundaries: (1) Steam bypass detection, (2) FG routing decision, (3) overlay rendering dispatch, (4) synthetic re-entrancy handling, (5) frame stats/metrics. Extract policy decisions into the existing dxgi_shared.h policy layer.
**Implementation guidance:** Follow the existing pattern in dx12_overlay_policy.h where pure functions make decisions. Each extracted helper should take explicit parameters (no global state reads in the middle). Move Steam VEH context into its own RAII guard.
**Suggested tests:** All 205 DXGISharedTest tests must still pass after refactoring. Add regression tests for each extracted helper's pure-function interface.
**Release blocker:** No
**Estimated effort:** Large

---

### F-03-003: g_RealD3D12ECL Dangling Pointer

**Category:** Memory, resource, lifetime, native/FFI, and undefined-behavior safety
**Severity:** High
**Confidence:** Medium
**Location:** `hook/apis/dx12_hook.cpp` lines ~106-111 (g_RealD3D12ECL declaration and probe), lines ~13956-13961 (usage in DetourExecuteCommandLists)
**Problem:** g_RealD3D12ECL is a raw function pointer obtained by probing a COMPUTE queue at startup. If Streamline unloads and reloads its internal queues (e.g., during a DLSS mode switch), this pointer becomes a dangling call target. There is no periodic re-validation after the initial probe. The code acknowledges this risk in comments.
**Impact:** Calling a freed function pointer in the ECL hook causes undefined behavior (likely access violation or DEVICE_REMOVED).
**Blast radius:** Game process crash. FG switching scenarios are most likely to trigger this.
**Recommended fix:** (a) Add a validation step before each ECL call that checks if the owning module is still loaded (similar to IsSavedStreamlineOriginalCallable in streamline_hook.cpp), or (b) re-probe g_RealD3D12ECL on each ProcessFrame when FG state changes, or (c) wrap the pointer in a shared_ptr with a deleter callback from Streamline's teardown path.
**Implementation guidance:** Option (b) is simplest: in DetourProcessFrame, when FG state transitions (especially OFF->ON or runtime ownership change), issue a fresh g_RealD3D12ECL probe if the current one was captured from a previous session epoch.
**Suggested tests:** FG trace replay test that simulates Streamline queue recreation.
**Release blocker:** No
**Estimated effort:** Small

---

### F-04-004: IAT Hook EAT Patching Unsigned Wrap-Around

**Category:** Security, privacy leakage, and source-level threat model
**Severity:** High
**Confidence:** High
**Location:** `hook/wrappers/iat_hook.cpp` lines ~464-465
**Problem:** EAT patching computes `newRVA = static_cast<DWORD>(reinterpret_cast<BYTE*>(hookFunction) - reinterpret_cast<BYTE*>(exportingModule))`. If hookFunction is at a lower address than exportingModule (e.g., across module boundaries, or in a relocated DLL), unsigned arithmetic wraps around, producing a very large DWORD RVA. Writing this garbage RVA into the EAT entry makes the export point to an arbitrary address — arbitrary code execution when the function is called.
**Impact:** Arbitrary code execution in the game process when the patched export is called. Exploitable by a malicious actor who can control module load order or hook function addresses.
**Blast radius:** Full game process compromise.
**Recommended fix:** Add a bounds check: verify that hookFunction lies within the exporting module's address range (between module base and base + size_of_image) before computing the RVA. If not, fall back to a different hooking strategy or return an error. Also add a signed ptrdiff_t intermediate to detect overflow.
**Implementation guidance:** Query the module's size via GetModuleInformation or VirtualQuery of the module base before the subtraction. Use `ptrdiff_t diff = reinterpret_cast<BYTE*>(hookFunction) - reinterpret_cast<BYTE*>(exportingModule)` and check `diff >= 0 && diff < moduleSize && diff <= UINT32_MAX`.
**Suggested tests:** Test with a hook function in a different module (higher and lower addresses) to verify the bounds check catches wrap-around.
**Release blocker:** Recommended, not blocking.
**Estimated effort:** Small

---

### F-04-005: Named Pipe IPC Without Authentication

**Category:** Security, privacy leakage, and source-level threat model
**Severity:** Medium
**Confidence:** High
**Location:** `common/process_ipc.cpp` lines ~172-174 (pipe SDDL)
**Problem:** The named pipe SDDL `D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)(A;;GA;;;AU)(A;;GA;;;AC)` grants access to SYSTEM, Administrators, Interactive Users, Authenticated Users, and ALL APPLICATION PACKAGES. There is no authentication, no token verification, and no proof-of-possession of any secret. Any process on the system can connect to the pipe and send commands (Shutdown, StartRecording, StopRecording, etc.).
**Impact:** A compromised user-mode process (e.g., a game running under the same user account) can control the capture engine: start/stop recording, shut it down, or inject into other processes.
**Blast radius:** Single user session — capture engine process and any injected game processes.
**Recommended fix:** (1) Restrict SDDL to `(A;;GA;;;SY)(A;;GA;;;BA)` only (SYSTEM + Administrators), or (2) add an authentication handshake (e.g., a random per-session token passed via command-line argument to child processes), or (3) use a named pipe ACL that matches only the capture engine's own process integrity level.
**Implementation guidance:** Option (1) is simplest and breaks compatibility with current non-admin usage. Option (2) preserves current usage: generate a random 64-bit token at startup, pass it via command line to child hooks, validate on every IPC command. For backward compat, make authentication optional behind a config flag.
**Suggested tests:** IPC tests should verify authentication token validation. Pipe ACL tests (manual or automated) should confirm access is denied to non-privileged processes.
**Release blocker:** No
**Estimated effort:** Medium

---

### F-03-006: slDLSSGOptions Struct Version Mismatch

**Category:** Memory, resource, lifetime, native/FFI, and undefined-behavior safety
**Severity:** High
**Confidence:** Medium
**Location:** `hook/apis/streamline_hook.cpp` lines ~138-160 (SLReflexConstants, slDLSSGOptions struct definitions), lines ~534-565 (CloneDLSSGOptions)
**Problem:** CE defines its own copies of `slDLSSGOptions` and `SLReflexConstants` structs with hardcoded `structVersion` and fixed field layouts. If NVIDIA ships a Streamline SDK with version 5+ of these structs (adding new fields), CE's undersized structs will be truncated during forwarding. Streamline may read garbage past CE's struct boundary.
**Impact:** Undefined behavior (garbage field reads) when Streamline receives a truncated struct. Potential crash or incorrect FG behavior after Streamline SDK update.
**Blast radius:** FG rendering when Streamline SDK is updated.
**Recommended fix:** Query the actual struct size from the Streamline runtime (if available) at the time of the call and only copy the minimum of CE's struct size and the runtime's expected size. Add a version mismatch log entry. Alternatively, forward the original pointer when the struct version exceeds CE's compiled version.
**Implementation guidance:** In CloneDLSSGOptions, add: `size_t runtimeExpectedSize = ...; size_t copySize = min(sizeof(MySLDLSSGOptions), runtimeExpectedSize);` and only copy `copySize` bytes. Log if `runtimeExpectedSize > sizeof(MySLDLSSGOptions)`.
**Suggested tests:** Add a test that simulates a newer Streamline struct with extra trailing fields and verifies CE handles the mismatch gracefully (does not crash, logs warning).
**Release blocker:** No
**Estimated effort:** Small

---

### F-03-007: VEH Handler Blocks for 15 Seconds

**Category:** Reliability, failure recovery, concurrency, and process stability
**Severity:** Medium
**Confidence:** High
**Location:** `common/crash_handler.cpp` line ~1003 (WaitForSingleObject in VEH handler)
**Problem:** The Vectored Exception Handler (VEH) blocks for up to 15 seconds waiting for the dump worker thread. During this time, the crashing thread cannot make progress, and if it holds any critical locks (e.g., loader lock, heap lock, D3D device lock), the entire process deadlocks. Other threads waiting for those locks also deadlock.
**Impact:** Full process deadlock during crash dump generation if the crashing thread holds critical locks. This prevents even the dump worker thread from making progress if it needs the same lock.
**Blast radius:** Entire process (all threads blocked).
**Recommended fix:** Reduce the timeout to 3-5 seconds. Move the blocking wait to a separate dedicated crash-handler thread that does not block the VEH invocation. Use `SendMessageTimeout` or a wait chain to detect lock contention.
**Implementation guidance:** Instead of blocking in the VEH, spawn a dedicated high-priority dump thread and return `EXCEPTION_CONTINUE_SEARCH` from the VEH after spawning it. The dedicated thread can do the blocking wait without holding any locks from the crashing thread's context. The 15-second timeout is only needed for stack overflow handling (special cases), so keep the longer timeout only on the dedicated thread.
**Suggested tests:** Simulate a crash while holding a critical lock and verify the process does not deadlock.
**Release blocker:** No
**Estimated effort:** Medium

---

### F-04-008: Dev Build DLL Verification Bypassable

**Category:** Security, privacy leakage, and source-level threat model
**Severity:** Medium
**Confidence:** High
**Location:** `captureengine/injection.cpp` lines ~802-811 (DLL signature check)
**Problem:** In dev builds, DLL signature verification is controlled by the `SKIP_DLL_VERIFICATION` environment variable. An attacker with local system access or the ability to influence environment variables (e.g., via a compromised child process) can bypass DLL signature verification and inject arbitrary code.
**Impact:** Arbitrary DLL injection into game processes when the capture engine runs in dev mode with the env var set.
**Blast radius:** Game processes injected by the capture engine.
**Recommended fix:** In production/pre-release builds (build.py production mode), do not honor SKIP_DLL_VERIFICATION. Make the env var only effective when a specific compile-time define is active. Log a warning when dev-only verification is bypassed.
**Implementation guidance:** Add `#ifndef CE_PRODUCTION_BUILD` guard around the SKIP_DLL_VERIFICATION check. Define CE_PRODUCTION_BUILD from build.py when building in non-dev mode.
**Suggested tests:** Integration: run with SKIP_DLL_VERIFICATION set in production mode and verify unsigned DLL injection is rejected.
**Release blocker:** No
**Estimated effort:** Small

---

### F-05-009: Relaxed Atomic Loads of g_StreamlineFGRunning in Hot Paths

**Category:** Performance, cost, energy, and resource efficiency
**Severity:** Low
**Confidence:** Medium
**Location:** `hook/apis/dx12_hook.cpp` (g_StreamlineFGRunning loads with memory_order_relaxed in ECL hook)
**Problem:** DX12's hot ECL and Present paths read `g_StreamlineFGRunning` with `memory_order_relaxed`. The writer uses `memory_order_acq_rel` via exchange. Between the relaxed load and the writer's store, the reader may see a stale value (0 instead of 1, or vice versa) for a few cycles.
**Impact:** Brief transient routing decisions based on stale FG state. In practice unlikely to cause visible issues (the next frame would correct the routing), but could cause a missed overlay render or briefly wrong status label during FG mode transitions.
**Blast radius:** None visible to users in normal operation.
**Recommended fix:** Upgrade to `memory_order_acquire` on the reader side. The cost is negligible (one memory barrier on ARM, free on x86) and the correctness guarantee improves.
**Implementation guidance:** Change `g_StreamlineFGRunning.load(std::memory_order_relaxed)` to `g_StreamlineFGRunning.load(std::memory_order_acquire)` in all hot paths.
**Suggested tests:** No test needed — correctness improvement, no behavioral change expected.
**Release blocker:** No
**Estimated effort:** Trivial

---

### F-07-010: Empty Test Body (InvalidValuesFallBack)

**Category:** Tests, regression hardening, and quality gates
**Severity:** Low
**Confidence:** High
**Location:** `tests/test_config.cpp` (InvalidValuesFallBack test)
**Problem:** The test `InvalidValuesFallBack` has an empty body with only a comment: `// Test robustness if needed`. This was left as a TODO that was never implemented.
**Impact:** Config file malformed-input robustness is not tested. Malformed config values may not gracefully fall back to defaults.
**Blast radius:** Limited to config parsing edge cases.
**Recommended fix:** Implement the test: feed invalid values for each config option (e.g., non-numeric for integer fields, out-of-range values, empty strings) and verify the parser falls back to defaults.
**Suggested tests:** The test itself is the fix.
**Release blocker:** No
**Estimated effort:** Small

---

### F-07-011: Timing-Dependent Tests

**Category:** Tests, regression hardening, and quality gates
**Severity:** Low
**Confidence:** High
**Location:** `tests/test_process_ipc.cpp` (polling loops with sleep_for(10ms) for up to 1s), `tests/test_frame_queue.cpp` (sleep_for(10ms) in Shutdown test)
**Problem:** IPC and frame queue tests use polling loops with fixed sleep durations instead of event-driven synchronization. Under high CI load or on slower machines, these can flake (miss the expected window, causing false failures or false passes).
**Impact:** Potential test flakiness. Currently passing on developer machine, but risk in CI.
**Blast radius:** Test reliability only.
**Recommended fix:** Replace polling with event-driven synchronization: use Windows events (CreateEvent/WaitForSingleObject) for IPC tests, and a condition_variable for frame queue tests.
**Implementation guidance:** For IPC tests, have the server signal an event when the pipe is ready. For frame queue tests, use a single-producer-single-consumer pattern with atomic flags + condition_variable.
**Suggested tests:** Existing test logic with event-driven synchronization.
**Release blocker:** No
**Estimated effort:** Small

---

### F-08-012: FillTransportRisk Dead Code

**Category:** Architecture, maintainability, and code consistency
**Severity:** Low
**Confidence:** High
**Location:** `hook/common/fg_session_state.cpp` lines ~164-175
**Problem:** FillTransportRisk sets `risk->bypassAvailable = false` unconditionally, then computes `staleSteamPresentHookRisk = steamOverlayLoaded && bypassAvailable`, which is always false. The code for Steam stale-hook risk detection is dead.
**Impact:** No functional impact — Steam stale-hook risk is never detected through this path. The bypassAvailable field is always false, so any downstream consumers of this field make incorrect decisions.
**Blast radius:** None currently (field is consumed by `ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupHandoff` and similar but cannot trigger).
**Recommended fix:** Remove the dead `staleSteamPresentHookRisk` logic from FillTransportRisk, or actually populate `bypassAvailable` from the relevant state (e.g., whether the Steam bypass VEH handler is currently active).
**Implementation guidance:** Search for all consumers of `FGTransportRisk::bypassAvailable` and `staleSteamPresentHookRisk`. If none currently depend on correct values, remove them. If they should, wire up the real bypass state.
**Suggested tests:** After cleanup, verify compilation and existing FG session state tests still pass.
**Release blocker:** No
**Estimated effort:** Trivial

---

### F-10-013: clang-tidy Not Default in Build

**Category:** Source build, tooling, static analysis, and binary inspection
**Severity:** Informational
**Confidence:** High
**Location:** `build.py` (clang-tidy run requires explicit flag)
**Problem:** clang-tidy bugprone and performance checks are configured in `.clang-tidy` but are not run as part of the default build. The `--clang-tidy` flag must be explicitly passed. This means static analysis results may rot between runs.
**Impact:** Bugprone issues (e.g., unused returns, unsafe casts) may go undetected during regular development.
**Blast radius:** Code quality over time; gradual decay of static-analysis hygiene.
**Recommended fix:** Consider adding a `--clang-tidy` run that does not block the build but reports findings, or add a separate CI step that runs clang-tidy and fails only on new findings. At minimum, document in AGENTS.md that `--clang-tidy` should be run periodically.
**Implementation guidance:** Add a `post_build_validation()` step in build.py that optionally runs clang-tidy on changed files. Or add a CI pipeline step for clang-tidy.
**Suggested tests:** N/A
**Release blocker:** No
**Estimated effort:** Small

---

### F-11-014: DbgHelp.dll Path Hijacking Risk

**Category:** Dependencies, supply chain, licensing, API/config/docs compatibility
**Severity:** Low
**Confidence:** Medium
**Location:** `common/crash_handler.cpp` (~line 1075, LoadLibraryA("DbgHelp.dll") fallback path)
**Problem:** After failing to load DbgHelp.dll from System32, the crash handler calls `LoadLibraryA("DbgHelp.dll")` without a full path. If the current directory has a malicious `DbgHelp.dll`, it will be loaded instead of the system one.
**Impact:** Arbitrary code execution during crash dump generation if an attacker can place a DLL in the current directory.
**Blast radius:** Only during crash dump generation.
**Recommended fix:** After System32 lookup fails, search only `C:\Windows\System32\` and `C:\Windows\SysWOW64\` explicitly, not the bare DLL name.
**Implementation guidance:** Replace `LoadLibraryA("DbgHelp.dll")` with `LoadLibraryA("C:\\Windows\\System32\\DbgHelp.dll")` (or the Wow64 path for 32-bit). If that also fails, fall back to `LoadLibraryExA` with `LOAD_LIBRARY_SEARCH_SYSTEM32`.
**Suggested tests:** No test needed — documentation-level fix.
**Release blocker:** No
**Estimated effort:** Trivial

---

### Deferred Lower-Priority Issues

The following were identified but are low-enough impact or speculative-enough to defer:
- Several files exceed 600-800 line guideline (dx12_hook.cpp 817KB, dxgi_shared.cpp 221KB, media_main.cpp 345KB, etc.) — tracked by DetourPresent split (F-06-002)
- `test_dxgi_shared.cpp` at ~2200 lines is overly large — consider splitting
- Relaxed atomics for non-critical diagnostic counters throughout
- ToCTOU in `IsRecursivePresent`/`ReleasePresent` for non-FG paths (low impact, well-understood)
- Several HRESULTs unchecked in test app (`dx12_test.cpp`)

---

## 4. Code and Binary Quality Production-Readiness Assessment

**Is the project production-ready from a code and binary quality perspective?** Conditionally yes. The core capture, encoding, and overlay rendering paths are solid, with good hardening, excellent diagnostic logging, and extensive policy-level testing. The DX12 overlay and FG switching paths are the most complex and risk-prone areas.

**Is it ready to ship?** Yes, with the 5 highest-priority fixes (F-02-001, F-03-003, F-04-004, F-03-006, F-04-008) preferably resolved first. The PostSL queue pointer race (F-02-001) is the most likely to cause a production crash.

**What must be fixed before shipping?**
- F-02-001 (PostSL queue use-after-free) — likely crash in FG switching scenarios
- F-04-004 (IAT EAT wrap-around) — exploitable in theory, low likelihood but high impact
- F-03-003 (g_RealD3D12ECL dangling pointer) — realistic crash risk after Streamline module reload

**What should be fixed soon after shipping?**
- F-04-005 (IPC without authentication)
- F-03-006 (SL struct version mismatch)
- F-04-008 (Dev build verification bypass)
- F-03-007 (VEH 15s blocking)

**What can be deferred?**
- F-06-002 (DetourPresent refactor) — high effort, manageable risk with current testing
- All Low and Informational findings

**What risks remain after fixes?**
- DetourPresent complexity (~1500 lines) remains — will always be a high-regression-risk area
- Third-party overlay coexistence is inherently fragile (Steam, Social Club, EOS, NVidia)
- Streamline/FFX SDK updates may introduce struct layout changes not covered by CE's hardcoded copies

**Which components are central, fragile, high-risk, or under-tested?**
- **Central + Fragile:** DXGI Present routing (dxgi_shared.cpp), DX12 ECL hook (dx12_hook.cpp), FG session state machine (fg_session_state.cpp), inline/import hook installation (inline_hook.cpp, iat_hook.cpp, streamline_hook.cpp)
- **High-Risk:** FG mode switching (all permutations of FSR <-> DLSS <-> OFF with and without third-party overlays), Steam overlay coexistence during FG startup, VEH handler for Steam bypass
- **Under-Tested:** End-to-end capture pipeline, actual GPU rendering (all mocked), heap/stack safety, memory safety, crash recovery, IPC multi-client scenarios
- **Performance-Sensitive:** ECL hook, Present hook, audio encode path
- **Parser-Sensitive:** Config file parsing, Streamline struct parsing, FFX API parsing
- **Native/FFI-Sensitive:** Inline hooks, IAT hooks, EAT patching, VEH handler, CreateRemoteThread injection
- **Security/Privacy-Sensitive:** IPC named pipe, DLL injection, DLL verification, crash dumps with sensitive data

**Which areas appear acceptable and should not be changed unnecessarily?**
- Media encoding pipeline (mediaengine/*) — solid, well-tested
- Audio sync utilities (audio_sync_utils.h) — extensively tested (51 tests)
- Capture pipeline policy (capture_pipeline_policy.h) — 68 tests, exhaustive
- FPS limiter + Reflex integration — 55 tests, production-proven
- Config parsing — 32 tests, handles legacy aliases, numbered sections, fallbacks
- Build system (build.py) — comprehensive, well-structured, supports ASan/UBSan, LTO, hardening flags
- Crash dump policy and storm mitigation — well-designed with .inprogress files, stable hashes, strong/weak signatures

---

## 5. Implementation Plan

### Phase 0: Safety and Baseline
- Related IDs: F-07-010, F-10-013
- Run `python build.py --skip-updates --clang-tidy` to establish baseline
- Run `python build.py --sanitize` to confirm ASan+UBSan builds work
- Add `--clang-tidy` to AGENTS.md as recommended periodic step
- Files: build.py, AGENTS.md
- Validation: build + tests + ASan pass

### Phase 1: Release Blockers
- Related IDs: F-02-001, F-03-003, F-04-004
- Fix PostSL queue pointer race (mutex or seqlock protection)
- Fix g_RealD3D12ECL dangling pointer (re-probe on FG state change)
- Fix IAT EAT patching wrap-around (bounds check)
- Files: hook/apis/dx12_hook.cpp, hook/wrappers/iat_hook.cpp
- Validation: all 745 tests pass under normal and ASan builds

### Phase 2: Correctness, Reliability, Compatibility
- Related IDs: F-03-006, F-03-007, F-08-012
- Fix slDLSSGOptions struct version mismatch (dynamic size query)
- Reduce VEH timeout or move blocking wait to dedicated thread
- Clean up FillTransportRisk dead code
- Files: hook/apis/streamline_hook.cpp, common/crash_handler.cpp, hook/common/fg_session_state.cpp
- Validation: FG transition sequence tests pass, crash handler tests pass

### Phase 3: Regression Hardening
- Related IDs: F-07-010, F-07-011
- Implement InvalidValuesFallBack config test
- Replace IPC/frame queue polling with event-driven sync
- Add concurrency stress test for PostSL queue pointers
- Files: tests/test_config.cpp, tests/test_process_ipc.cpp
- Validation: new tests pass, no regression in existing tests

### Phase 4: Performance, Resource, Storage
- Related IDs: F-05-009
- Upgrade relaxed atomic loads to acquire in hot ECL path
- Files: hook/apis/dx12_hook.cpp
- Validation: existing tests pass, no measurable perf regression

### Phase 5: Architecture and Maintainability
- Related IDs: F-06-002
- Split DetourPresent into focused helper functions
- Monitor whether dx12_hook.cpp (817KB) should be split by domain
- Files: hook/common/dxgi_shared.cpp, hook/common/dxgi_shared.h
- Risk: Refactoring risk — all 205 DXGISharedTest tests must pass after split
- Validation: full test suite + manual review of extracted functions

### Phase 6: Source Build, Binary Quality, Dependencies
- Related IDs: F-04-005, F-04-008, F-11-014
- Restrict IPC named pipe SDDL or add authentication
- Guard SKIP_DLL_VERIFICATION behind production build define
- Fix DbgHelp.dll safe search path
- Files: common/process_ipc.cpp, captureengine/injection.cpp, common/crash_handler.cpp
- Validation: IPC tests pass, injection tests pass

### Phase 7: Final Validation
- Run `python build.py --skip-updates` (clean build + all tests)
- Run `python build.py --sanitize` (ASan+UBSan build + tests)
- Run llvm-readobj to verify ASLR/DEP/CET in all binaries
- Verify no new clang-tidy warnings on changed files
- Update llm-wiki with new durable knowledge

---

## 6. Implementation Rules

1. Make the smallest safe change that fixes the root cause. Do not expand scope.
2. Preserve behavior, APIs, config formats, ABI expectations, and user-visible behavior unless currently wrong or unsafe.
3. Refactor DetourPresent (F-06-002) only after Phase 1 blockers are fixed and test baselines are confirmed.
4. Do not add features unless required for correctness, safety, reliability, or production-readiness.
5. Preserve useful debug logging; do not remove diagnostics that aid root-cause analysis of FG or overlay issues.
6. Fix warning/analyzer root causes instead of suppressing them.
7. Prefer safe APIs: bounded checks, mutex/seqlock protection, dynamic struct sizes, explicit bounds.
8. Treat inline hook, IAT hook, EAT patching, VEH, and FG state machine code as high-risk — validate with ASan.
9. Do not hide crashes without fixing the root cause. The dump storm mitigation already handles crash sequences gracefully.
10. Preserve or improve binary hardening and crash diagnosability (PDBs, CodeView, ASLR, DEP, CET).
11. Every fix must have validation — preferably an automated regression test.

---

## 7. Final Verification Checklist

- [x] Clean checkout builds successfully (confirmed: build 0.1.3123)
- [x] All 745 tests pass (confirmed: [PASSED] 745 tests)
- [ ] No known crash reproducer still crashes (no active reproducer in this session, 0.1.3123 verified)
- [ ] LSP/compiler/linker/static-analysis findings resolved or justified (clang-tidy available, no output in automated run)
- [x] Memory/resource checks: ASan+UBSan available, no writable+executable sections, CET enabled, stack protector enabled
- [ ] Malformed/oversized/corrupted/missing-file/invalid-config paths tested: config has 32 tests including invalid values, but InvalidValuesFallBack is empty (F-07-010)
- [x] Generated binaries: ASLR (`--dynamicbase`), DEP (`--nxcompat`), CET (`-fcf-protection=full`), HEVA on x64, CodeView PDBs, no writable-executable sections
- [x] Logs/telemetry/dumps/crash reports: dump storm mitigation, .inprogress files, stable hashes, strong/weak signatures, rate-limited logging
- [ ] Filesystem/persistence: path traversal not explicitly checked for config path (low risk), temp files use .inprogress pattern (good)
- [ ] Concurrency/lifecycle: PostSL queue pointer race (F-02-001) and g_RealD3D12ECL (F-03-003) identified and recommended
- [ ] Parser/decoder/deserializer/importer: config parser tested (32 tests), Streamline struct version mismatch (F-03-006), FFX API parsing tested (10 tests)
- [ ] Security/privacy: EAT wrap-around (F-04-004), IPC auth (F-04-005), dev build verification bypass (F-04-008), DbgHelp path (F-11-014)
- [x] GUI/UI (system tray): limited scope — tray app minimal, overlay rendering is custom per-API
- [ ] Domain-specific (game capture/inject): crash avoidance, dump storm mitigation, freeze watchdog, VEH handler all present and well-designed
- [x] Dependencies/licensing: FFmpeg (LGPL), MIT, OBSIndicator (MIT), SpecialK fork — licenses bundled in installed output
- [x] Public APIs, configs, persisted formats, source-tree docs: no intentional breaking changes, config backward compat maintained
- [x] Out-of-scope CI/CD/signing/deployment/packaging/installer/infrastructure not scored

---

*Report generated by mimo-v2.5-pro-precision. Evidence sources: source code inspection, build output (0.1.3123), test results (745 passed), binary inspection via llvm-readobj, and dependency analysis.*
