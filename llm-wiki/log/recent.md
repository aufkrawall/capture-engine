# llm-wiki Log

### 2026-08-20 - The Witcher 3 crashed on start because CE hooked a d3d11.dll it did not own

Both renderers crashed within a second of the DX11 hook install, in two places that turned out to be one
bug. Three sessions, all DX12 (`bin\x64_dx12\`), all build 0.1.6181:

- `20260820_031021` (PID 5420) and `witcher3crash` - `capture_hook_x64!DX11Hook::Init+0xd9d`
  (`dx11_hook.cpp:439`), `movzx eax, byte ptr [rdi]` with `rdi = 0x7ffd84422470`, `ds:...=??`. That address
  is `d3d11!D3D11CreateDeviceAndSwapChain` - the log line `DX11: Hook target at 00007FFD84422470` names it
  855 ms earlier - and `lm` in the dump lists **no d3d11 module at all**. The image had been unmapped.
- `20260820_023643` and `20260820_031021` (PID 7660) - `ntdll!RtlpWaitOnCriticalSection+0xb3`,
  `inc dword ptr [rax+24h]` with `rax = 0`, reached from
  `DX11Hook::Init` -> `d3d11!D3D11CreateDeviceAndSwapChain` -> `D3D11CoreCreateDevice` ->
  `CCreateDeviceCache::CUMDAdapterCache::Load` -> `nvldumdx!OpenAdapter10_2` -> `nvwgf2umx`. `DebugInfo`
  is `nullptr` on a contended section: an all-zero `CRITICAL_SECTION`, i.e. one whose owner was torn down.

**Root cause.** `GetModuleHandle` returns a *non-owning* handle. Something in the process (Witcher 3 next-gen
loads XeSS/`XeFX_Loader`, Streamline and NGX, any of which probe D3D11) loaded d3d11.dll, and
`CheckAndInstallHooks` committed to the whole DX11 install on that bare presence signal - `DX11 check #10:
dllPresent=1 => INSTALL`, one tick after `dllPresent=0`. About a second later the probe's owner released the
module. Whichever half of `DX11Hook::Init` CE happened to be in decided the crash signature: reading the
export's entry byte faulted on the unmapped image, and being inside `D3D11CreateDeviceAndSwapChain` faulted
because d3d11's `DLL_PROCESS_DETACH` had already destroyed `CCreateDeviceCache` and dropped the NVIDIA UMD
underneath CE's thread. Same race, two landing points.

**Fix (`hook/common/module_pin.{h,cpp}`, `module_pin_policy.h`).** A module CE inline-patches must be
reference-held, because CE never withdraws the patch or the "original" pointers taken alongside it.
`ce::module_pin::PinByName` resolves with `GET_MODULE_HANDLE_EX_FLAG_PIN`, which also closes the
check-then-use window: `GetModuleHandleEx` runs under the loader lock and either pins a live module or
reports none. Applied where export addresses are captured for the process lifetime:

- `iat_hook_init.cpp` - `Initialize{DXGI,D3D12,D3D11,D3D10,D3D9,DDraw}Hooks` cache export addresses in
  globals *and* hand them to game code through `RegisterDynamicHook`. This is the central choke point: every
  later `GetModuleHandleA("dxgi.dll")` in the tree now resolves an already-pinned image.
- `custom_hook.cpp` `HookExport` (saves `*original` from the module), `dx11_hook.cpp` `DX11Hook::Init`.
- `main_install.cpp` pins *before* constructing `g_DX11Hook`, and leaves the hook unset if every candidate
  vanished in between - latching a no-op install would mean DX11 hooks could never be retried.

**Deliberately not pinned: every hook target.** `InlineHook::InstallImpl` and `CreateBypassTrampoline` also
dereferenced entry bytes blind, but their targets include Streamline/NGX/FFX plugins that the FG paths
genuinely load and unload (`streamline_hook_resolve.cpp` answers that with scoped holds and a teardown
generation counter). Those two call sites therefore *validate* with `ce::module_pin::IsReadableCode` -
committed, executable, readable, walking every region the range touches so a protection split inside a
`.text` cannot cause a false refusal - and change no third-party lifetime. `dx11_hook.cpp`'s new
`TryReadCreateEntryBytes` does pin, because the D3D11/D3D10 create entry it inspects is also the pointer CE
stores and calls for the rest of the process, even when a loader-injected proxy owns it.

**Tests.** `tests/test_module_pin.cpp`: policy truth table (unmapped query, `MEM_RESERVE`, `PAGE_NOACCESS`,
`PAGE_GUARD`, data pages, execute-without-read, ranges leaving the region), live probes against real code,
data and freed memory, a two-page allocation split by `VirtualProtect` to prove the region walk, and
source invariants over all five fixed call sites plus the two that must stay pin-free.

**Confirmed by Windows itself.** The Application event log for 03:12:24 names it exactly: faulting module
`capture_hook_x64.dll`, exception `0xc0000005`, offset `0x00000000000f8e0d`.

**What is behind it, and is NOT ours.** With build 0.1.6182 the same title still failed to start
(session `20260820_142322`), but with a different shape: no access violation, no CE frame - REDengine threw
an unhandled C++ exception (`0xE06D7363`) on the main thread ~8.7 s in and its own `crashreporter.exe` took
over. d3d11.dll never loaded in that run, so the fixed path was never even entered. **The user then
reproduced the same failure with CE not injected at all**, so this second failure is the game's, not CE's.
Two observations that looked incriminating and are not: no D3D12 device was ever created in the process
(no `nvwgf2umx`/`nvldumdx`), and it ran on `C:\Windows\System32\D3D12Core.dll` although witcher3.exe
exports `D3D12SDKVersion`/`D3D12SDKPath` and ships `bin\x64_dx12\D3D12\D3D12Core.dll` - both follow from
the game aborting before its renderer came up. Also ruled out: `nv_lod_spread_fix` (patches only
`nvoglv64/32.dll`, the GL/Vulkan ICD, and writes nothing to disk - the installed driver still verifies as
`Valid`), and a broken machine (`installed/testapp/dx12_test.exe` creates a D3D12 device fine).

**A deliberate non-change.** CE builds a throwaway D3D12 device at startup for its Present-hook bootstrap,
which does make CE - not the game - the caller that settles the process's D3D12 runtime state, Agility SDK
selection included. That is a real hazard worth revisiting, but gating it changes the DX12 Present-hook
bootstrap that the overlay, Steam coexistence, DLSS FG, FSR FG and Streamline all sit on, and there is no
evidence it fixes anything here. It was written, then reverted; only the diagnostics were kept.

**Diagnostics kept.** `HookSwapchainVTableViaTempSwapchain` abandoned three first-order failures in silence
- factory, device and command-queue creation - which is why the session log shows "Installing Present hooks
eagerly" followed by 65 ms of nothing. All three now report their HRESULT
(`tests/test_dx12_temp_swapchain_diagnostics.cpp`).

**Pending:** real Witcher 3 runs on both renderers, once the game starts standalone again. Not yet
re-examined: whether committing the full DX11 install - temp device *and* swapchain - off nothing but
`d3d11.dll` being present is right in a process already known to be D3D12. The lifetime bug made it fatal;
it is still avoidable work.
