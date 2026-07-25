# llm-wiki Log — Archive 2026-W20b

### 2026-05-13 — DX10 32-bit overlay fix (build 0.1.3085 → 0.1.3086)

- **Input**: Overlay appeared in DX10 64-bit test app but not in DX10 32-bit test app. The 32-bit `hook_debug.log` (181 lines) was much shorter than the 64-bit one (585+ lines), missing all `CreateSwapChainForHwnd` / swapchain wrapping / overlay init.
- **Root cause (v1 fix, build 3085 — insufficient)**: Extended 32-bit condition to `d3d12DeviceCreated || WasD3D11Or10DeviceCreated()` but the timing was wrong — `CheckAndInstallHooks()` ran in DllMain before any device existed, so both were false. The DX12 hook init was deferred to the HookThread's 1-second periodic retry (`main.cpp:2153-2159`), but by then the swapchain already existed. `InstallGlobalVTableHooks()` ran too late.
- **Root cause (v2 fix, build 3086)**: The 32-bit path set `shouldInitDX12Hook` to a device-creation-dependent check. During DllMain, no device has been created yet, so the condition was always false on first call. The HookThread retry loop re-evaluates `CheckAndInstallHooks()` every 1 second (`main.cpp:2153`), but by the time `WasD3D11Or10DeviceCreated()` becomes true (~1.2s), the DX10 app has already created its swapchain.
  - 64-bit was fine: `shouldInitDX12Hook = true` unconditionally, so `InstallGlobalVTableHooks()` runs synchronously in DllMain before any game code.
- **Fix**: Changed 32-bit `shouldInitDX12Hook` to `true` unconditionally (matching 64-bit). The original third-party overlay interference concern (nvspcap.dll) is handled inside `DX12Hook::Init()` which defers only the eager Present hook install (`HookSwapchainVTableViaTempSwapchain()`) when nvspcap.dll is present, NOT `InstallGlobalVTableHooks()`.
- **Timeline for 32-bit DX10**:
  1. t=0ms — DllMain: `shouldInitDX12Hook = false` (no device yet) → skip
  2. t=700ms — HookThread: same → skip
  3. t=1254ms — Game thread: `D3D10CreateDevice` → `g_D3D11Or10DeviceCreated = true`
  4. t=2040ms — HookThread 1s tick: now `WasD3D11Or10DeviceCreated()` true → DX12 hook init runs — **too late!**
- **Source anchor**: `hook/main.cpp:1617-1629`
- **Verification**: `python build.py --skip-updates` passed. `python build.py --no-build --run-tests --skip-updates` passed (unit_tests).

### 2026-05-13 — GTA V Enhanced DLSS FG startup stall and dump storm fix (build 0.1.3084)

- **Input**: GTA V Enhanced DX12 still hung/crashed after starting with all FG off and enabling DLSS FG in-game (`installed/captureengine/logs/20260513_083935`). The earlier patched-`dxgi!Present+0x5` crash route was avoided, but the fresh runtime-owned Streamline handoff starved PostSL startup activation: bypass transport could return before `ProcessFrame` saw the authoritative swapchain, later activation paths had stale/`nullptr` targets, and Streamline/Rockstar external dumps repeated.
- **Startup activation fix**:
  - DX12 now retains the authoritative runtime-owned Streamline startup activation swapchain on `CreateSwapChainForHwnd` handoff and before any startup-handoff bypass return, with an AddRef'd pointer released on confirmation, FG off, resize/invalidation, lifecycle reset, replacement, or shutdown.
  - A shared callback slot lets Streamline request `DX12_TryInvokePostSLStartupActivationCallback(source, clearStartupWindow)` without depending on DX12 internals. The service acquires a retained/fresh non-null swapchain, clears the startup window only immediately before a real callback, invokes PostSL with that swapchain, and releases the local ref.
  - Streamline flush/ECL-expiry paths no longer invoke PostSL with `nullptr`; deferred OFF churn that remains startup-protected after window expiry services the activation helper instead of looping forever.
- **Dump storm fix**:
  - Mirrored external dumps and supplemental CE dumps now write through `.inprogress` files and rename only after success/non-empty validation; mirrors no longer overwrite existing session `.dmp` files.
  - Mirrored/supplemental external dump filenames include a short stable hash of the external source path to avoid collisions.
  - External dump gating is keyed by process id, dump basename, exception code/address/thread when available. Strong signatures produce at most one mirror and one supplemental CE dump; repeated duplicates log suppression. If the same strong signature hits at least 3 times within 30 seconds after a supplemental dump was captured, CE marks process termination and calls `TerminateProcess` once. Weak signatures may dedupe but never force termination.
  - Freeze-watchdog dump capture is now one-shot per run and also uses temp/rename; ordinary watchdog freeze dumps still do not force-kill the game.
- **Source anchors**:
  - `hook/apis/dx12_hook.h:88-89`, `hook/apis/dx12_hook.cpp:1209-1329`, `hook/apis/dx12_hook.cpp:4333-4337`, `hook/apis/dx12_hook.cpp:4483`, `hook/apis/dx12_hook.cpp:14081`, `hook/apis/dx12_hook.cpp:14719`
  - `hook/common/dxgi_shared.h:78`, `hook/common/dxgi_shared.cpp:200`, `hook/common/dxgi_shared.cpp:1673`, `hook/common/dxgi_shared.cpp:1758`, `hook/common/dxgi_shared.cpp:2445`, `hook/common/dxgi_shared.cpp:2523`
  - `hook/apis/streamline_hook.cpp:58`, `hook/apis/streamline_hook.cpp:2692`
  - `hook/common/dx12_overlay_policy.h:1589`, `hook/common/dx12_overlay_policy.h:1601`
  - `common/crash_dump_policy.h:123`, `common/crash_dump_policy.h:243-275`, `hook/main.cpp:94-191`, `hook/main.cpp:204-290`
  - `hook/common/freeze_watchdog.h:19`, `hook/common/freeze_watchdog.h:112`, `hook/common/freeze_watchdog.cpp:324-369`, `hook/common/freeze_watchdog.cpp:579`
- **Regression coverage**:
  - `tests/test_dxgi_shared.cpp:2382-2411` covers retained activation-swapchain policy, retained-preference conditions, and deferred OFF churn activation servicing.
  - `tests/test_crash_dump_policy.cpp:99-127` covers strong/weak external dump signatures, duplicate suppression expectations, and termination thresholds.
  - `tests/test_fps_limiter.cpp:886-889` covers watchdog one-shot dump policy.
  - `tests/test_crash_handler.cpp:118-119` keeps binary regression strings for retained Streamline startup activation and external dump storm termination in the built hook DLL.
- **Verification**:
  - `python build.py --skip-updates` passed.
  - Focused run passed: `python build.py --no-build --run-tests --skip-updates --gtest-filter="DXGISharedTest.*Streamline*:DXGISharedTest.*Bypass*:DXGISharedTest.*PostSL*:DX12FGTraceReplayFixture.*:CrashHandlerBinaryTest.*:CrashDumpPolicyTest.*:StreamlineRuntimePolicyTest.*"` (179 tests).
  - Full run passed: `python build.py --no-build --run-tests --skip-updates` (718 tests).
- **Stale-risk / runtime validation still needed**: Re-run GTA V Enhanced DX12 all-FG-off -> DLSS FG in-game, DLSS/FSR/off switching both directions, Talos DX12 UE5 with Steam/Streamline, Strange Brigade DX12 Steam, and one DX11 Steam title. The fix is generic startup activation + dump hygiene, not a GTA-specific branch.

### 2026-05-13 — Default process priority changed to `above_normal`; `copy_queue_priority` dead code fix (build 0.1.3072→0.1.3073)

- **`process_priority` default changed** from `normal` to `above_normal` for the Media (video capture) sub-process. Rationale: Windows 11 favors foreground windows for CPU scheduling; the background Media process benefits from a modest scheduling boost.
  - `common/config.cpp:756` default changed
  - `common/config.cpp:499` embedded template updated
  - `captureengine/config.ini.template:56` updated
- **`copy_queue_priority` dead code fix**: The config value was parsed from config.ini and written to shared memory (`SetCopyQueuePriority`) but never read back — it had zero effect. Added consumption code in `InitOverlaySync()` at `hook/apis/dx12_hook.cpp:6589-6595` that reads `g_pSharedMem->GetCopyQueuePriority()` and sets `queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH` when value == 2 (`high`).
- **Files changed**:
  - `common/config.cpp` (2 changes: default + template)
  - `captureengine/config.ini.template` (1 change: default)
  - `hook/apis/dx12_hook.cpp` (new: copy_queue_priority consumption)
  - `llm-wiki/performance-priority.md` (new page)
  - `llm-wiki/index.md` (added entry)
  - `llm-wiki/log/recent.md` (this entry)
- **Follow-up (same session)**: Added `WGCCapture::SetGpuPriority()` to apply the existing `gpu_priority` config to the WGC capture D3D11 device (both shared and dedicated paths). Previously only the encoder device received GPU thread priority.
  - `captureengine/wgc_capture.h:180` — added `void SetGpuPriority(int priority)` public method
  - `captureengine/wgc_capture.cpp:2592-2621` — implementation via `IDXGIDevice::SetGPUThreadPriority`
  - `captureengine/media_main.cpp:738` — call in `StartWgcRecordingCapture`
- **Verification**: `python build.py --skip-updates` passed. `python build.py --no-build --run-tests --skip-updates` passed 709/709 tests.

### 2026-05-12 — Talos Reawakened (UE5 DX12) early crash at startup: final fix — lazy-exec trampoline pool (build 0.1.3055->0.1.3058)

- **Root cause**: Trampoline pool was PAGE_EXECUTE_READWRITE always. sl_interposer/Steam overlay enumerates committed executable pages during init, finds our pool, misreads its bytes as a COM vtable → writes pool address into D3D12Core NDXGI::CDevice vtable pointer → crash at RIP=0 via `jmp [rax+N]` through NULL.
- **VEH `EXCEPTION_CONTINUE_EXECUTION` failure (0.1.3055)**: Patching NULL entries with E_NOINTERFACE stub and setting `RIP = RAX` caused `ret` to pop 0 from stack (no return address was pushed by `jmp [rax+N]`) → infinite VEH loop → no crash dump written.
- **Final fix**: **Lazy-exec trampoline pool**. The pool is now allocated as `PAGE_READWRITE` (non-executable) instead of `PAGE_EXECUTE_READWRITE`. A VEH handler in `CrashHandlerExceptionFilter` catches the DEP page fault when a thread first tries to execute from the pool, makes the pool `PAGE_EXECUTE_READWRITE`, retries the faulting instruction, and lets subsequent calls work without faulting. This prevents memory scanners from finding the pool as executable memory.
- **Changes**:
  1. `hook/wrappers/inline_hook.h`: Added `IsInTrampolinePool()` and `SetTrampolinePoolProtection()` public API.
  2. `hook/wrappers/inline_hook.cpp`: AllocateTrampolinePool now uses `PAGE_READWRITE`. During `Install()`, pool is temporarily `PAGE_EXECUTE_READWRITE` for construction, then reverted to `PAGE_READWRITE`. Implemented `IsInTrampolinePool` and `SetTrampolinePoolProtection`.
  3. `common/crash_handler.cpp`: Removed dangerous `EXCEPTION_CONTINUE_EXECUTION`. Added lazy-exec VEH handler that catches DEP page faults on trampoline pool, makes it executable, retries. Kept diagnostic-only vtable corruption detection as fallback.
- **Source anchors**: `hook/wrappers/inline_hook.h:73-78` (public API), `hook/wrappers/inline_hook.cpp:517-545` (PAGE_READWRITE alloc), `hook/wrappers/inline_hook.cpp:547-576` (IsInPool/SetProtection), `hook/wrappers/inline_hook.cpp:1310-1325` (temp RWX during construct), `hook/wrappers/inline_hook.cpp:1456-1463` (revert to RW), `common/crash_handler.cpp:530-549` (lazy-exec VEH handler).
- **Verification**: `python build.py --skip-updates` passed (build 0.1.3058). `python build.py --no-build --run-tests --skip-updates` passed 698/698 tests.

- **FINAL root cause**: The trampoline pool is allocated as `PAGE_EXECUTE_READWRITE` and stays that way forever. sl_interposer and/or Steam overlay are known to enumerate committed executable pages (via `VirtualQuery`, memory scanning) during init. They find the PAGE_EXECUTE_READWRITE trampoline pool and **misinterpret its contents as a COM vtable table**. The pool address (0x00007FFDD9BC0223 deterministically) gets written into a D3D12Core NDXGI::CDevice object's vtable pointer field (first 8 bytes). When D3D12Core!CLayeredObject::QueryInterface dispatches through the corrupted vtable, it reads NULL → `jmp [rax+N]` → RIP=0.
- **Why prior fixes didn't work**:
  - Inline hook write ordering (0.1.3047): addressed a race condition that wasn't the crash cause (the crash is deterministic, same RAX every run).
  - DX12_HookDeviceVTable vtable boundary guard (0.1.3049): `DX12_HookDeviceVTable` was **never reached** before the crash — the crash happens INSIDE sl_interposer's slSetD3DDevice before any CE vtable hook code runs.
- **Fix**: Added VEH-based crash recovery in `CrashHandlerExceptionFilter` (crash_handler.cpp:517-578). When RIP=0 DEP crash is detected with RAX in the high x64 address range (0x00007Fxxxx range typical for DLLs/hooks), the handler:
  1. Scans 64 vtable entries starting from RAX
  2. Patches any NULL entries with a 6-byte stub that returns E_NOINTERFACE (0x80004002)
  3. Sets RIP to execute the first patched stub and returns `EXCEPTION_CONTINUE_EXECUTION`
  4. The COM dispatch fails gracefully instead of crashing
  5. Logs diagnostics: "Trampoline crash recovery: patched N NULL vtable entries at RAX=0x..."
- **Source anchor**: `common/crash_handler.cpp:517-578` (VEH recovery logic).
- **Still needed**: Real root-cause fix would make trampoline pool PAGE_READWRITE after construction (toggle to EXECUTE_READWRITE only during trampoline building). This is deferred because it requires careful re-engineering of the pool protection model.
- **Verification**: `python build.py --skip-updates` passed (build 0.1.3050). `python build.py --no-build --run-tests --skip-updates` passed 698/698 tests. Manual test: game should no longer hard-crash at startup (VEH handler recovers). Check crash logs for recovery messages.

- **CORRECTED root cause**: `DX12_HookDeviceVTable` (dx12_hook.cpp:14259-14292) assumed all D3D12 devices have 23+ vtable entries. When sl_interposer wraps the D3D12 device during `slSetD3DDevice`, the wrapper has an **incomplete vtable** with fewer than 23 slots. CE reads `vtbl[22]` (CreateSampler) which is **past the end** of the SL wrapper vtable. `VTableHook::Create` then writes the `DetourCreateSampler` address to `&vtbl[22]`, which is actually **adjacent memory** — often another COM object's vtable pointer. This overwrites the adjacent vtable pointer with the trampoline pool address → deterministic RIP=0 crash later (identical RAX=0x00007FFDD9BC0223 every run).
- **First fix attempt (0.1.3048) wrongly blamed inline hook race condition** — the crash is deterministic (same RAX every run, stack shows D3D12Core QI→sl_interposer chain), not a race.
- **Fix — 3 defensive guards**:
  1. `DX12_HookDeviceVTable`: Added module validation via `GetModuleHandleExA(FROM_ADDRESS)` on the vtable pointer. If vtable is in `sl_interposer` or `sl.common` module, skip the hook (log: "Skipping CreateSampler vtable hook for SL wrapper device"). Also added `!vtbl[22]` NULL check before patching.
  2. `VTableHook::Create`: Added NULL-entry guard — if the vtable entry is NULL, skip without patching and return `ErrorNotExecutable`. Prevents writing past vtable boundary when the vtable is incomplete.
  3. `DX12_HookQueueVTable`: Added `!vtbl[10]` NULL check before the ECL vtable hook. Same pattern protection for command queue vtables.
- **Source anchors**: `hook/apis/dx12_hook.cpp:14258-14306` (device vtable fix), `hook/wrappers/vtable_hook.cpp:38-49` (NULL entry guard), `hook/apis/dx12_hook.cpp:14238-14250` (queue vtable fix).
- **Verification**: `python build.py --skip-updates` passed (build 0.1.3049). `python build.py --no-build --run-tests --skip-updates` passed 698/698 tests. Manual: Talos Reawakened with CE inject + Steam overlay required.

- **Input**: `logs/20260512_170336` — Talos1-Win64-Shipping.exe (64-bit Steam UE5 DX12). Crashes ~2s after injection, before main window visible. Steam overlay + CE overlay + Streamline/DLSS FG + NVNGX + FFX/FG all loading concurrently.
- **Crash dump analysis** (`crash_20260512_170341_399_pid4520_tid17256.dmp`):
  - `0xC0000005` DEP at `0x0000000000000000` (RIP=0)
  - `RAX=0x00007FFDD9BC0223` — points into `capture_hook_x64.dll`'s trampoline pool region
  - `RCX→sl_interposer`, `RDX→sl_reflex` — crash during concurrent Streamline/NVNGX init
  - Crash thread TID 17256 = DXGI factory wrapper thread (game thread doing device creation)
  - Stack completely empty (RetAddr=0), consistent with `jmp [vtable+N]` through NULL
- **Root cause**: Race condition in `InlineHook::Install()` (hook/wrappers/inline_hook.cpp):
  1. **`WriteJump()` (line 601-630)**: Wrote 6-byte JMP header (`FF 25 00 00 00 00`) BEFORE the 8-byte absolute target address. A concurrent thread executing the trampoline decodes `JMP [RIP+0]` and reads 0s from the not-yet-written address bytes → jumps to NULL.
  2. **Target patch byte-loop (lines 1467-1495)**: Wrote 14-byte detour JMP via `for (int i = 0; i < PATCH_SIZE; i++) pTarget[i] = jmpBuf[i]` — byte-by-byte, no ordering guarantee. A concurrent game thread executing the target function decodes a partially-written JMP → reads garbage displacement → jumps to NULL.
  3. **No FlushInstructionCache on trampoline**: The trampoline was written but never cache-flushed. On weak memory-model or multi-socket systems, a concurrent thread could execute stale i-cache lines from the trampoline.
  4. **Vulnerable period**: ~2s from process start, UE5 does massive parallel init (DXGI factory+adapters, D3D12 device, Streamline+NVNGX+FFX DLL loading), while CE's HookThread installs hooks on all these modules concurrently. The race window is much wider than in simpler games.
- **Fix — 3 changes in `hook/wrappers/inline_hook.cpp`**:
  1. **`WriteJump()` (line 601-625)**: Changed write order to: 8-byte target address FIRST, `MemoryBarrier()`, then 6-byte JMP header. If a concurrent thread sees a partial JMP, `dest+6` already contains the correct absolute target — `[RIP+0]` reads the correct address. Same fix applied to x86 path (displacement first, opcode second).
  2. **`Install()` target patch (lines 1469-1518)**: Replaced byte-by-byte loop with two `memcpy` calls: write 8-byte address at pTarget+6 first, `MemoryBarrier()`, then write 6-byte JMP header. Same fix applied to x86 path (4-byte displacement as `*(int32_t*)` write first, opcode second).
  3. **Trampoline FlushInstructionCache** (line 1446-1449): Added `FlushInstructionCache(GetCurrentProcess(), trampoline, trampolineOffset)` after trampoline is fully built but before target is patched.
- **Fix — VEH diagnostic in `common/crash_handler.cpp`** (lines 747-775): Added DEP-at-RIP=0 detection in `CrashHandlerExceptionFilter`. When faultAddr=0 with accessType=2 (DEP), logs full RAX/RCX/RDX/R8/R9/RSP context with "possible trampoline race condition" annotation.
- **Why this is a root-cause fix**: The write ordering guarantees that any concurrent thread reading a partially-written JMP sees either original code (safe) or a JMP that decodes a valid target address. The code change is minimal (no new code paths, no removed features, no disabling).
- **Source anchors**: `hook/wrappers/inline_hook.cpp:600-632` (WriteJump fix), `hook/wrappers/inline_hook.cpp:1446-1449` (trampoline flush), `hook/wrappers/inline_hook.cpp:1469-1518` (target patch fix), `common/crash_handler.cpp:747-775` (VEH diagnostic).
- **Verification**: `python build.py --skip-updates` passed (build 0.1.3047). `python build.py --no-build --run-tests --skip-updates` passed 698/698 tests (build 0.1.3048). Manual test: Talos Reawakened with CE inject + Steam overlay required.

## Activity Timeline

### 2026-05-12 — BioShock Infinite (UE3 DX11 x86) crash at swapchain creation: CWrapD3D11Device must not wrap IDXGIDevice queries (build 0.1.3044->0.1.3046)

- **Input**: `logs/20260512_164151` — BioShockInfinite.exe (32-bit Steam UE3 DX11). Crash on first swapchain creation, 3 seconds after launch.
- **Crash dump analysis** (`crash_20260512_164210_920_pid13240_tid12904.dmp`):
  - `0xC0000005` READ at `0x0000007F` in `dxgi!CDXGIBaseAdapter::InternalGetAdapterDesc+0x14`
  - `ecx=0xFFFFFFFF` (invalid `this` pointer for adapter)
  - Instruction: `add eax, dword ptr [ecx+80h]` reads at `0x7F` — near-NULL
  - Stack: `gameoverlayrenderer!OverlayHookD3D3+0x11574` → `dxgi!CDXGIFactory::CreateSwapChain` → `CreateSwapChainImpl` → `CDXGISwapChain::CDXGISwapChain` → `UpgradeSwapEffect` → `CDXGIBaseAdapter::InternalGetAdapterDesc` ← CRASH
- **Root cause**: `CWrapD3D11Device::QueryInterface(IID_IDXGIDevice)` returned a `CWrapDXGIDevice` wrapper. When real dxgi code (called through Steam overlay's hook) QI'd our wrapped device for IDXGIDevice, it got a CWrapDXGIDevice which then returned a CWrapDXGIAdapter via `GetAdapter()`. Real dxgi code internally casts this to `CDXGIBaseAdapter*` and accesses members at fixed offsets — but the wrapper has a different memory layout → reads garbage (`0xFFFFFFFF`) → crash at `[this+0x80]`.
- **Fix** (`hook/wrappers/d3d11_device_wrap.cpp:143-157`, `hook/wrappers/d3d10_device_wrap.cpp:104-112`):
  - Changed IID_IDXGIDevice handling in both wrappers from wrapping to direct forwarding: `return m_pReal->QueryInterface(riid, ppvObj)` instead of creating `CWrapDXGIDevice` → `CWrapDXGIAdapter`.
  - CE only needs wrapped D3D11 device (for sampler state / pixel shader interception) and wrapped DXGI factory (for swapchain creation interception). DXGI device/adapter objects derived from D3D11 device QI must not be wrapped — real DXGI code consumes them internally and expects real `CDXGIBaseAdapter` memory layout.
- **Safety verified**: CE's own code that queries IID_IDXGIDevice from the wrapped device (VRAM via GetDesc, SystemMetrics via LUID) works identically with real DXGI objects since it only calls `GetAdapter()->GetDesc()`.
- **Source anchors**: `hook/wrappers/d3d11_device_wrap.cpp:143-157` (D3D11 fix), `hook/wrappers/d3d10_device_wrap.cpp:104-112` (D3D10 fix for consistency).
- **Verification**: `python build.py --skip-updates` passed (build 0.1.3045). `python build.py --no-build --run-tests --skip-updates` passed 698/698 tests. Build 0.1.3046.
