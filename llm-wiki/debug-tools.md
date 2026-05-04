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