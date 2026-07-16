## Windows debugging and binary analysis tools

- When analyzing crash dumps, use the correct symbol path that includes both the Microsoft symbol server AND the local PDB directory:
```
cdb -z crash.dmp -y "srv*;%USERPROFILE%\Programme\build\captureproject\installed\captureengine" -c ".ecxr; k; q"
```
The `srv*`-only path misses CE's local PDBs and produces incomplete stack traces.

- Installed Windows tools for `.dmp`, symbol, PE/COFF, Sysinternals, and media/capture analysis:

| Tool | Purpose | Installed/default path |
| --- | --- | --- |
| `cdb.exe` | Command-line `.dmp` debugging and stack inspection | `C:\Program Files\Windows Kits\10\Debuggers\x64\cdb.exe` |
| `windbg.exe` | Interactive `.dmp` debugging | `C:\Program Files\Windows Kits\10\Debuggers\x64\windbg.exe` |
| `WinDbgX.exe` | Interactive WinDbg Preview `.dmp` debugging | `%LOCALAPPDATA%\Microsoft\WindowsApps\WinDbgX.exe` |
| `dumpchk.exe` | Validate dump readability and basic dump metadata | `C:\Program Files\Windows Kits\10\Debuggers\x64\dumpchk.exe` |
| `symchk.exe` | Verify/download symbols for binaries and dumps | `C:\Program Files\Windows Kits\10\Debuggers\x64\symchk.exe` |
| `dbh.exe` | Inspect symbols and PDB contents | `C:\Program Files\Windows Kits\10\Debuggers\x64\dbh.exe` |
| `pdbcopy.exe` | Copy/strip PDBs for symbol handling | `C:\Program Files\Windows Kits\10\Debuggers\x64\pdbcopy.exe` |
| `symstore.exe` | Add/query files in a symbol store | `C:\Program Files\Windows Kits\10\Debuggers\x64\symstore.exe` |
| `gflags.exe` | Configure debug/runtime flags; use only with explicit intent | `C:\Program Files\Windows Kits\10\Debuggers\x64\gflags.exe` |
| `umdh.exe` | Heap snapshot and leak investigation | `C:\Program Files\Windows Kits\10\Debuggers\x64\umdh.exe` |
| `dumpbin.exe` | Inspect PE/COFF headers, imports, exports, sections, symbols, and disassembly | `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\dumpbin.exe` |
| `undname.exe` | Undecorate MSVC C++ symbols | `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\undname.exe` |
| `link.exe /dump` | `dumpbin`-style fallback inspection | `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\link.exe` |
| `lib.exe /list` | List static library contents | `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\lib.exe` |
| `editbin.exe` | PE/COFF mutation; do not use unless explicitly requested | `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\editbin.exe` |
| `procdump.exe` | Capture process dumps | `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Microsoft.Sysinternals.Suite_Microsoft.Winget.Source_8wekyb3d8bbwe\procdump.exe` |
| `procmon.exe` | Trace process, registry, file, and network activity | `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Microsoft.Sysinternals.Suite_Microsoft.Winget.Source_8wekyb3d8bbwe\procmon.exe` |
| `procexp.exe` | Inspect processes, handles, DLLs, and threads | `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Microsoft.Sysinternals.Suite_Microsoft.Winget.Source_8wekyb3d8bbwe\procexp.exe` |
| `vmmap.exe` | Inspect process virtual memory layout | `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Microsoft.Sysinternals.Suite_Microsoft.Winget.Source_8wekyb3d8bbwe\vmmap.exe` |
| `handle.exe` | Find open handles | `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Microsoft.Sysinternals.Suite_Microsoft.Winget.Source_8wekyb3d8bbwe\handle.exe` |
| `listdlls.exe` | List loaded DLLs for a process | `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Microsoft.Sysinternals.Suite_Microsoft.Winget.Source_8wekyb3d8bbwe\listdlls.exe` |
| `sigcheck.exe` | Inspect signatures, versions, hashes, and VirusTotal metadata | `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Microsoft.Sysinternals.Suite_Microsoft.Winget.Source_8wekyb3d8bbwe\sigcheck.exe` |
| `strings.exe` | Extract printable strings from binaries or dumps | `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Microsoft.Sysinternals.Suite_Microsoft.Winget.Source_8wekyb3d8bbwe\strings.exe` |
| `ffmpeg.exe` | Media conversion/inspection helper for captures | `%USERPROFILE%\Programme\build\captureproject\build\msys64\clang64\bin\ffmpeg.exe` |
| `ffprobe.exe` | Media metadata/probing helper for captures | `%USERPROFILE%\Programme\build\captureproject\build\msys64\clang64\bin\ffprobe.exe` |
| `llvm-strings.exe` | Extract printable strings from COFF objects / DLLs (reliable on `.o`/`.dll` where `grep -a` mis-parses) | `%USERPROFILE%\Programme\build\captureproject\build\msys64\clang64\bin\llvm-strings.exe` |
| `llvm-objdump.exe` | Disassemble / inspect sections of the hook DLL/objects | `%USERPROFILE%\Programme\build\captureproject\build\msys64\clang64\bin\llvm-objdump.exe` |

## DX12 DRED GPU-fault diagnostics (device-hung / `0x887A0006`)

- The hook arms **DRED** (Device Removed Extended Data) auto-breadcrumbs + page-fault in `Wrapped_D3D12CreateDevice` before the game's device is created (`hook/common/dx12_dred.cpp`). It is the primary tool for any `DXGI_ERROR_DEVICE_HUNG/REMOVED` (e.g. the x86 DX12 focus/mode-switch freeze): a bare HRESULT is not actionable, DRED names the hung command list and the faulting GPU VA.
- **Default OFF (opt-in)**; enable page-fault-only mode with an empty `ce_dx12_dred` file or env `CE_DX12_DRED=pf`, and full breadcrumbs with `CE_DX12_DRED=1` / `full` only while actively diagnosing a real device-removal. Auto-breadcrumbs (`SetAutoBreadcrumbsEnablement(FORCED_ON)`) make the APPLICATION's every `ID3D12GraphicsCommandList::Reset()` allocate a breadcrumb buffer via a KERNEL GPU allocation (`NtGdiDdDDICreateAllocation/DestroyAllocation`); during the Alt+Tab iflip<->composited mode switch that stalls the present thread for seconds and itself trips the 2 s TDR — i.e. leaving full DRED on caused the very freeze it was meant to capture (`logs/20260606_145929`: `CGraphicsCommandList::Reset -> Dred::AllocateBreadcrumbBuffer -> NtGdiDdDDIDestroyAllocation2`, gap=2646ms). Decision via `ce::dx12_overlay_policy::DecideDredArmMode`. Freeze dumps still capture the CPU-side present-thread stack without DRED.
- On device removal (the two `ProcessFrame` device-removed sites and the freeze watchdog dump) the hook log (`installed/captureengine/logs/<ts>/hook_debug.log`) gets a block:
  - `DX12 DRED: ===== device-removed extended data (<reason>) =====`
  - `DX12 DRED:  node#N queue='...' list='...' completedOps=X/Y  <-- INCOMPLETE (GPU hung in this list)` plus the breadcrumb op window and `ctx@opN=` context strings.
  - `DX12 DRED:  pageFaultVA=0x...` then `[existing]` and `[recently-freed]` allocation names. A faulting VA that matches a **recently-freed** allocation is the smoking gun for stale-resource access (e.g. a backbuffer reallocated during the iflip<->composited mode switch). CE overlay objects are named `CE_OverlayFence` / `CE_OverlayCmdList` / `CE_OverlayAlloc[i]` / `CE_OverlayQueue` / `CE_OverlayOffscreenRT`.
- Confirm DRED is actually armed in a build with: `llvm-strings installed/captureengine/capture_hook_x64.dll | grep "DX12 DRED: armed"`. If absent, the arming was dead-stripped (ThinLTO + `--gc-sections`) — the DRED entry points must stay `__declspec(dllexport)` (`CE_DRED_API`; plain `used` was honored on x64 but stripped on x86).
- DRED auto-breadcrumbs require arming BEFORE device creation. CE arms in `DX12Hook::Init()` (early, on a worker thread); the `Wrapped_D3D12CreateDevice` site is dead in normal builds (`#ifdef ENABLE_D3D12_WRAPPER`, no `d3d12_wrappers.dll`). If injection happens after the game's device is already created, DRED can't arm and `GetAutoBreadcrumbsOutput1` returns failure.
- **Historical v12 upload-ring investigation**: an INCOMPLETE CE overlay command list (`DRAWINDEXEDINSTANCED`) with `pageFaultVA=0` and a render thread parked in the NVIDIA allocation/map path originally motivated per-slot overlay fencing. The fence fixes a real upload-buffer reuse hazard and remains an invariant: never reuse a slot until the GPU completes its frame. That signature alone does **not** prove a ring stomp; the deterministic x86 hang later reproduced with correct fencing and was isolated to font-resource text draws.
- **Current x86 DX12 no-vsync fixed signature (v13)**: healthy 32-bit `dx12_test.exe` logs show `DX12 focus-loss sync policy=v13 draw-every-frame + x86 solid-span text + upload-slot per-frame fence`, `DX12 Overlay: x86 solid-span text path enabled`, and `DX12 DIAG: Texture2D command ... textured=0`. A reappearance of `textured=1` in the x86 no-FG path is a regression.

## DX12 always-on present/ECL timing diagnostics (`DX12 DIAG:` in hook_debug.log)
Built-in, ALWAYS-ON (no env/flag/install), written via `HookLogImportant` to `hook_debug.log`. Added 2026-06-06 to localize x86 DX12 present/ECL stalls; see `dx12-overlay-third-party-coexistence.md` and `handoff-dx12-32bit-crash.md` for the current v13 fixed state. Source: `hook/apis/dx12_hook.cpp`, `hook/common/custom_overlay_dx12.cpp`, and `hook/common/dxgi_shared.cpp`.

- `DX12 DIAG: ExecuteCommandLists SLOW Xms (queue=.. overlayQueue=.. lists=.. devRemoved=0x..)` — a single ECL ≥2 ms (the call includes the real forward where the NV driver's `AllocateCB → NtGdiDdDDICreateAllocation` happens). `devRemoved` non-zero = post-removal spinning, ignore. One early run maxed at 9.5 ms, while a later mid-stall watchdog dump caught an app ECL blocked in the NVIDIA GPU-VA map path; treat the timing window as run-specific rather than using it alone to assign root cause.
- `DX12 DIAG: ECL timing/1s: count=.. maxMs=.. avgMs=..` — per-second ECL stats for steady-state 32-bit vs 64-bit comparison (note: count/avg inflate AFTER a freeze because the app spins on the dead device).
- `DX12 DIAG: overlay footprint draws=.. vbBytes=.. ibBytes=..` — CE's per-frame overlay GPU work (was identical 32-bit vs 64-bit: draws=4 vb=13760 ib=2064 → not a code-path difference).
- `DX12 DIAG: DetourPresent TOTAL SLOW Xms` / `ProcessFrame (overlay) SLOW Xms` / `overlay-completion wait SLOW Xms` — present-phase split. **Interpretation**: slow TOTAL with no slow ProcessFrame/wait localizes that sample to the real `Present`; slow `wait` implicates outstanding CE overlay work; slow `ProcessFrame` implicates CE record/submit work. These timings localize a stall but do not by themselves identify the failing draw shape or prove driver ownership.

## DX12 debug-layer diagnostic (env `CE_DX12_DEBUG_LAYER`)

- For cases DRED can only report as a "pure hang" (`pageFaultVA=0`, e.g. the x86 DX12 Alt+Tab overlay-draw hang), CE can enable the D3D12 debug layer to surface the exact resource-state/hazard at the API call. Requires the Graphics Tools optional feature (`C:\Windows\System32\d3d12SDKLayers.dll` — present on this machine). Armed in `DX12Hook::Init()` before device creation (`ce::dx12_dred::ArmDebugLayerBeforeDeviceCreation`).
- Levels: `CE_DX12_DEBUG_LAYER=1` enables the debug layer (lighter); `=2` also enables GPU-based validation (heavier, serializes — can mask timing hangs but catches GPU-side hazards). Unset/`0` = off (default; the debug layer changes timing so it is diagnosis-only).
- The device's `ID3D12InfoQueue` is drained to the hook log every `ProcessFrame` and on device-removal, tagged `DX12 DBGLAYER [<context>] sev=.. cat=.. id=..: <description>`. Run the repro with the env set, then read `hook_debug.log` for those lines around the freeze.
