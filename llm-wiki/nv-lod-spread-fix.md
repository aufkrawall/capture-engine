# NVIDIA LOD-Spread Quality Fix (`nv_lod_spread_fix`)

Last cross-checked: 2026-10-19 (moved out of `graphics-overrides-and-frame-pacing.md` unchanged; content last
hardware-validated 2026-09-02 against 32.0.16.1656).

Primary sources:
- `hook/ngx/nv_lod_spread_override.{h,cpp}`
- `hook/vulkan_layer/vulkan_layer_hooks.cpp`
- `hook/runtime/main_{hookthread,overlay_detect}.cpp`
- `tests/test_nv_lod_spread_override.cpp`

Related: [graphics-overrides-and-frame-pacing.md](graphics-overrides-and-frame-pacing.md) (the other cross-API
graphics overrides), [api-translation-overrides.md](api-translation-overrides.md) (which APIs the option touches).

## Mechanism and evidence

- The NVIDIA GL/VK ICD (`nvoglv64.dll` / `nvoglv32.dll`) still carries the `FERMI_UNOPT_LOD_SPREAD` driver setting.
  Unlike D3D10, 11, and 12, the Vulkan driver does not correctly configure `FERMI_UNOPT_LOD_SPREAD` when changing
  texture filtering to High Quality (HQ); the worse quality of the Quality (Q) setting is erroneously kept with
  configured HQ filtering, emitting a LOD spread of `0` into the texture-filtering state record (header `0xA0030E46`)
  instead of `0x10`. The effect is that negative mip bias looks unnecessarily temporally shimmery, whereas positive
  mip bias looks unnecessarily blurry. The literal string `FERMI_UNOPT_LOD_SPREAD` is **not** in either DLL; the
  setting is identified by its ON/OFF enum payloads `0x37299934` / `0x56023627`.
- **Hardware finding that chose the mechanism**: the first implementation wrote the ON payload into the setting
  global, assuming that one-reader equivalence made it interchangeable with NOPing the branch. Session
  `20260808_154051` disproved the timing assumption: the Vulkan layer created the instance at `15:42:26.778` and
  device at `15:42:26.825`; the injected hook entered `DllMain` at `15:42:27.119` and wrote the global only at
  `15:42:27.361`. The log reported success, but Filter Tester DXVK retained the stock low-quality/shimmery result.
  The process-local fix must therefore apply the exact proven branch patch before device initialization.
- The writer is `mov [<global>], eax`, guarded by `test al,al / je`, immediately after
  `mov edx, 0x3001ac; call <settings accessor>`. So **`0x003001AC` is this setting's DRS id**, the store is
  conditional on that query succeeding, and the accessor is a single internal `.data` function pointer shared by
  ~1000 setting reads (1038 disp32 slots in 1088). It is *not* an import, so CE's IAT machinery cannot see it.
- The Vulkan layer reads the host-resolved `nvLodSpreadFix` bit from shared memory. It sweeps before
  `vkCreateInstance`, then again immediately after the next layer returns successfully - the ICD is mapped at that
  point, while `vkCreateDevice` has not run. A defensive sweep also occurs at device entry. The hook DLL retains
  OpenGL and already-loaded/reloaded-module coverage, but is not the owner of Vulkan's timing.
- Detection is pattern-based and self-validating, never fixed-offset: the resolved address must actually hold one of
  the two documented payloads, and the fallback site must be a short `jcc` whose two paths load table slots exactly
  four bytes apart with the ON slot on the fall-through side. Anything else is refused and logged. Verified against
  32.0.16.1088, 32.0.16.1656 (620.14) and 32.0.16.2012 (620.12), x64 and x86, resolving to the documented offsets in
  all of them; an already-neutralized branch is accepted only when the ON load, the skip jump, and the adjacent OFF
  table slot remain structurally provable.
- **The branch is neutralized by zeroing its `rel8` displacement, not by replacing the opcode pair with NOPs.**
  `jcc +0` continues at the fall-through - the path the scanner proved is ON - whichever way the flags go, so the two
  forms are equivalent, but zeroing writes one byte where NOPing writes two. That is the difference between a fix
  that applies and one that cannot: a two-byte replacement is atomic only while the pair fits an aligned word the
  process can compare/exchange, and no x86 CAS is wider than 64-bit (32-bit processes have no `cmpxchg16b` at all),
  so a pair at 7 modulo 8 had to fail closed. A single-byte store cannot tear at any address, so site alignment no
  longer decides whether the option does anything. `IsNeutralizedBranch` accepts both forms, because a driver patched
  on disk by the community tool, and any process patched by a CE build before 0.1.6368, carries `90 90`.
- Filter Tester DXVK x86 session `20260902_065516` is what forced that change. 32.0.16.1656 moved the 32-bit ICD's
  branch to `nvoglv32+0x576C27`; module bases are 64KB-aligned, so the site sits at 7 modulo 8 - the one alignment the
  two-byte writer refused. Hook and layer both resolved the site correctly and then logged `crosses the widest
  supported aligned atomic word (addressMod8=7) - nothing patched`, so `nv_lod_spread_fix=on` was a no-op and Vulkan
  kept the shimmery negative-LOD-bias result while D3D10/11/12 (which do not go through this ICD) looked correct.
  Nothing had regressed in CE; a driver update relocated the site by four bytes. The same driver's x64 branch is at
  `nvoglv64+0x4C413B` (3 modulo 8), which is why 64-bit titles were unaffected and the report looked API-specific.
- The injected hook and Vulkan layer each compile their own copy of the patcher. Their scans and protection changes
  are serialized through one per-process named mutex, so nested `VirtualProtect` calls cannot restore the code page
  to another copy's temporary writable protection. The complete aligned 32-bit word containing the displacement
  participates in `InterlockedCompareExchange`, so a concurrent adjacent-byte change loses the CAS rather than being
  overwritten; that word never straddles a page, so one `VirtualProtect` always covers the write. The instruction
  cache is flushed before the original protection is restored, and failures report whether bytes changed,
  cache-flush/protection state, and final byte verification. A structurally proven pre-patched driver is accepted
  without any live write.
- `nv_lod_spread_fix=off` (the default) arms nothing. The machine's driver files are never written, so they keep
  their NVIDIA signature, and other processes are unaffected. Caveat carried in `config.ini.template`: patching a
  graphics driver in memory is the kind of thing anti-cheat systems object to.
- The shared flag occupies one byte of the existing alignment gap between `msaaSamples` and `prerenderLimit`;
  compile-time offset/size assertions prove the IPC layout and ABI signature are unchanged. Host config updates,
  initial publication, the hook, and the Vulkan layer all consume the same resolved per-process value.
- Source anchors: `hook/ngx/nv_lod_spread_override.{h,cpp}`, the pre-device calls in
  `hook/vulkan_layer/vulkan_layer_hooks.cpp`, inject fallback in `hook/runtime/main_{hookthread,overlay_detect}.cpp`, and
  coverage in `tests/test_nv_lod_spread_override.cpp`.
- **Split-architecture titles are patched where the Vulkan device is, not where the game is.** NVIDIA RTX Remix runs
  the 32-bit game (`hl2.exe` for Portal RTX) as a thin client and does every bit of rendering in the 64-bit
  `NvRemixBridge.exe`, so no ICD is ever mapped into the game process. Session `20260902_071853` shows the override
  arming in `hl2.exe` with nothing to patch, which is correct, and the hook patching `nvoglv64+0x4C413B` in the
  bridge 0.7s before its `vkCreateInstance` and 1.1s before `vkCreateDevice`. The layer's own sweep then reported
  `already has the validated ON branch patch`, which is the cross-copy recognition of the zero-displacement form
  working between two separately compiled patchers.
- Diagnostics: `NV LOD spread: forced FERMI_UNOPT_LOD_SPREAD ON in ... (validated branch +0x...: 75 05 -> 75 00 via
  atomic 32-bit compare/exchange, check +0x..., setting 0x...)`, or an explicit structural-validation/write failure
  with post-write state. Hardware-validated on 2026-09-02 against 32.0.16.1656 in both directions: Filter Tester
  DXVK x86 (`nvoglv32+0x576C27`, user-confirmed image quality) and Portal RTX x64 (`nvoglv64+0x4C413B`).
