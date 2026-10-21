# Binary size

Last cross-checked: 2026-10-21 (0.1.7291 measured before and after the size fixes; FFmpeg and exe/DLL duplication
decisions open)

Primary sources:
- `tools/verify_pe_hardening.py` (`image_budget_errors`), `tools/build/build_common.py` (`append_windows_pdb_linker_flag`)
- `hook/pacing/pacing_trace.cpp`, `hook/metrics/performance_metrics.cpp`, `tools/compile_shaders.py`
- `tools/build/build_ffmpeg.py` (configure recipe), `tools/build/build_packaging.py` (package contents)

## Why it matters

The hook DLLs and the Vulkan layers load into every injected game. Their image size (`SizeOfImage`) is contiguous
address space in that process, which counts in 32-bit games, and their `.tls` template is copied once per thread
of the game, render thread or not. Everything else only costs disk and installer size.

## How to measure

- Sections: `build/msys64/clang64/bin/llvm-readobj.exe --file-headers --sections <image>`; `RawDataSize` is file
  size, `VirtualSize` beyond it is zero-fill (address space only).
- Who owns a section's bytes: `llvm-pdbutil dump -symbols <pdb>` lists `S_[LG]DATA32`/`S_[LG]THREAD32` with
  `addr = <section>:<offset>`; sorting by offset and taking gaps gives sizes (a gap can also cover unnamed data, so
  confirm big ones in source). `S_[GL]PROC32` records carry `code size` for `.text`. With LTO every function's module
  is the `.lto.obj`, so group by namespace instead.
- DWARF origin: `llvm-dwarfdump --debug-info <image>`, `DW_AT_producer` of each compile unit.

## Invariants (enforced after linking)

- A first-party image with a PDB carries no `.debug_*` sections; the PDB holds CE's debug info, first-party code
  emits CodeView only (`-gcodeview`), and the DWARF came from the static mingw-w64 CRT (GCC 16.2 objects on x86,
  clang on x64). `-Wl,--strip-all` next to `--pdb` drops it and keeps PDB, RSDS and `.buildid` (checked on a toy
  DLL and on the product images, 2026-10-21).
- At most 256 KiB initialized writable data and a 16 KiB TLS template per project image; sanitizer runs skip this.
  Large state belongs on the heap, allocated when its feature runs, or zero-filled (`.bss`). Watch for statics whose
  default member initializers are not all zero (`= UINT64_MAX`, `= -1`, `= true`): the whole object, array included,
  then lands in `.data`.
- Shader bytecode headers use `inline constexpr` arrays: `static const` gave the D3D11 and D3D12 translation units
  each a copy (about 57 KiB per hook DLL).

## Findings 2026-10-21 (0.1.7291, before the fixes)

| Image | File | Driver |
| --- | ---: | --- |
| `capture_hook_x86.dll` | 10.6 MiB | `.data` 5.66 MiB raw: `pacing_trace` rings (`Ring<49152>` 4.1 MiB + `Ring<16384>` 1.4 MiB, 88 bytes/slot); `.text` 3.0 MiB; DWARF EH `.eh_frame`+`.gcc_except_table` 0.72 MiB (i686 mingw ABI); CRT DWARF 0.54 MiB; `.reloc` 157 KiB (no RIP-relative addressing) |
| `capture_hook_x64.dll` | 10.1 MiB | same rings; `.text` 3.1 MiB; CRT DWARF 0.57 MiB |
| hooks + `VK_LAYER_CE_overlay*.dll` | | `.tls` 39-41 KiB per thread, 32 KiB of it `static thread_local std::array<float, 8192> frameTimes` (`ComputeWorstPercentileFPS`) |
| `avcodec-ce-63.dll` | 27.8 MiB | static libaom (~8.4 MiB text+rdata, encoder and decoder) and SVT-AV1 (~7.1 MiB); rest FFmpeg's own codecs |
| `captureengine.exe` / `cengine.dll` | 3.6 / 3.3 MiB | each links the same runtime code (MediaEncoderSession 231 KiB, WGCCapture, sensors, InjectionManager, DisplayTimingService, startup, role host); CRT DWARF ~0.58 MiB each |
| `mediaengine.dll` | 2.6 MiB | VideoEncoder/MediaEngine/std; CRT DWARF 0.55 MiB |
| `captureengine_elevation_service.exe` | 0.87 MiB | 0.50 MiB of it CRT DWARF |
| `VK_LAYER_CE_gate*.dll` | 0.47 MiB | about half CRT DWARF |

Installed tree: 36 MiB first-party images, 33 MiB FFmpeg, 119 MiB PDBs (shipped on purpose so field dumps are
actionable). Per-session symbol copies under `logs/*/symbols` are hard links into `symbol_store`, so they do not
multiply disk use (one x86 hook PDB had 41 links).

After the fixes (0.1.7293, `335ceccd`..`40f767f1`), file KiB before -> after:

| Image | File | Image (`SizeOfImage`) | TLS template |
| --- | ---: | ---: | ---: |
| `capture_hook_x86.dll` | 10858 -> 4600 | 12044 -> 5772 | 39.5 -> 7.4 |
| `capture_hook_x64.dll` | 10300 -> 4012 | 11700 -> 5404 | 40.7 -> 8.6 |
| `VK_LAYER_CE_overlay.dll` / `_x86` | 1680 / 2156 -> 1324 / 1798 | 2120 / 2616 -> 1752 / 2240 | 32 -> <1 |
| `VK_LAYER_CE_gate.dll` / `_x86` | 479 / 472 -> 232 / 260 | | |
| `cengine.dll`, `mediaengine.dll`, `captureengine.exe` | 3370, 2710, 3732 -> 2794, 2150, 3142 | | |
| `captureengine_elevation_service.exe` | 894 -> 384 | | |

First-party images 35.8 -> 20.2 MiB; the LZMS setup program 46.18 -> 44.32 MB (rings and DWARF compressed well).
What is left in the hooks is code (`.text` ~3 MiB), `.rdata` and, on x86, the DWARF EH tables and relocations.

## Open decisions (not taken; behavior or architecture changes)

- **SVT-AV1 in avcodec (~7 MiB):** no product code selects `libsvtav1`; only tests (`test_recording_recovery.cpp`)
  and an undocumented `encoder=libsvtav1` reach it. Screenshots use `libaom-av1`. Dropping it removes a reachable
  software encoder, needs an FFmpeg rebuild (`FFMPEG_BUILD_CONFIGURATION_VERSION` bump) and a test encoder swap.
- **libaom scope:** the screenshot encoder runs with realtime usage; a realtime-only libaom build would be smaller,
  but 10-bit 4:4:4 still-picture support and the tests' libaom decoder would have to be proven first.
- **`captureengine.exe` vs `cengine.dll` duplication (~1.4 MiB):** part of the libcengine role-host design
  (`library/11-helper-isolation.md`); deduplicating is architecture work, not a size fix.
- **x86 hook without LTO:** documented toolchain reason in `build_common.py` (`HOOK_OPT_FLAGS_X86`).
- **Zero-fill in the hook** (address space only): `pacing_health` ring 384 KiB, `d3d11_dispatch` retired table
  128-257 KiB.
