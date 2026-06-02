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
- Enabled by default; toggle with env `CE_DX12_DRED` (`0`/`off`/`false` disables). Auto-breadcrumbs add GPU overhead, so disable once a family is diagnosed.
- On device removal (the two `ProcessFrame` device-removed sites and the freeze watchdog dump) the hook log (`installed/captureengine/logs/<ts>/hook_debug.log`) gets a block:
  - `DX12 DRED: ===== device-removed extended data (<reason>) =====`
  - `DX12 DRED:  node#N queue='...' list='...' completedOps=X/Y  <-- INCOMPLETE (GPU hung in this list)` plus the breadcrumb op window and `ctx@opN=` context strings.
  - `DX12 DRED:  pageFaultVA=0x...` then `[existing]` and `[recently-freed]` allocation names. A faulting VA that matches a **recently-freed** allocation is the smoking gun for stale-resource access (e.g. a backbuffer reallocated during the iflip<->composited mode switch). CE overlay objects are named `CE_OverlayFence` / `CE_OverlayCmdList` / `CE_OverlayAlloc[i]` / `CE_OverlayQueue` / `CE_OverlayOffscreenRT`.
- Confirm DRED is actually armed in a build with: `llvm-strings installed/captureengine/capture_hook_x64.dll | grep "DX12 DRED: armed"`. If absent, the arming was dead-stripped (ThinLTO + `--gc-sections`) — the DRED entry points must stay `__attribute__((used, noinline))`.