# llm-wiki Log

### 2026-08-20 - CE installed Streamline 2.x hooks on a Streamline 1.x interposer

With the executable renamed past the driver refusal (see the entry below), The Witcher 3 started
and then died loading a save: `0xC0000005` reading `0x11C5F6F0` in `sl.common+0x2379D`
(`mov rax,[rdi]`), session `20260820_221409`. The stack is
`123.exe -> capture_hook_x64!Hooked_slEvaluateFeature -> sl_interposer!slEvaluateFeature+0x23c ->
sl_common`. Without CE injected the same save loads.

**Root cause.** `sl.interposer.dll` in The Witcher 3 is **1.5.6**, and Streamline 1.x and 2.x share
export names but not signatures:

| | 1.x | 2.x |
| --- | --- | --- |
| `slSetTag` | `(const Resource*, BufferType, uint32_t id, const Extent*) -> bool` | `(const ViewportHandle&, const ResourceTag*, uint32_t, CommandBuffer*) -> Result` |
| `slEvaluateFeature` | `(CommandBuffer*, Feature, uint32_t frameIndex, uint32_t id) -> bool` | `(Feature, const FrameToken&, const BaseStructure**, uint32_t, CommandBuffer*) -> Result` |

CE mirrored the 2.x shapes and installed them unconditionally. On 1.x the evaluate hook therefore
took the game's `ID3D12GraphicsCommandList*` out of RCX into its `uint32_t feature` parameter and
handed the truncated 32-bit half straight back to Streamline as the command buffer - `0x11C5F6F0`
is the low half of a live command list. The 1.x prologue proves the shape:
`mov esi,r9d / mov r14d,r8d / mov edi,edx / mov r15,rcx`, forwarded verbatim at `+0x22d`, returning
`bool` in AL. `slSetTag` was the same bug one arming away: it walks `tags[i]` out of what 1.x passes
as a small `BufferType` enum, and only survived because DLSS-G never armed the UI-tag path.

**Fix (0.1.6192).** `hook/common/streamline_api_generation.h` classifies the interposer from
generation-exclusive exports - 1.x has `slSetFeatureConstants`, `slGetFeatureSettings`,
`slSetFeatureEnabled`, `slIsFeatureEnabled`, `slGetFeatureConfiguration`; 2.x has `slSetTagForFrame`,
`slGetNewFrameToken`, `slGetFeatureRequirements`, `slSetD3DDevice`, `slGetFeatureFunction`,
`slIsFeatureLoaded`. `slSetTag` and `slEvaluateFeature` may never classify anything, because they are
in both. Both markers present, or neither, is `Unknown` and installs nothing ABI-sensitive; CE never
guesses a foreign calling convention. The classification gates the inline hooks, the IAT patches and
the GetProcAddress-time dynamic routes, which is why the two ABI-sensitive dynamic registrations
moved out of `RegisterDynamicHooksOnce` into `RegisterAbiSensitiveDynamicHooksOnce`.

`hook/apis/streamline_hook_v1.cpp` restores the capability rather than only removing the crash:
1.x-shaped `slSetTag`/`slEvaluateFeature` hooks that forward their arguments verbatim and return
`bool`. Because 1.x `slSetTag` carries no command buffer, the UI record is deferred to the next
`slEvaluateFeature`, which supplies one and still runs before the present DLSS-G consumes.
1.x also has no `slSetD3DDevice`, so the 2.x route's "Streamline accepted a D3D12 device" signal
never fires; the first evaluate whose command buffer answers as an `ID3D12GraphicsCommandList`
proves the same thing and arms the same `BeginPreactivationStandby(2)`.

**The 1.x `sl::Resource` layout is proven per resource, never assumed:** 48 bytes committed and
readable, `type == eResourceTypeTex2d`, `native` non-null and pointer-aligned with a readable vtable
whose first slot is executable, `state` a subset of the defined `D3D12_RESOURCE_STATES` bits, and
finally a real `QueryInterface` to `ID3D12Resource` with a `TEXTURE2D` desc. A mislaid offset
produces "no record", never a barrier from a garbage state. `eBufferTypeUIColorAndAlpha` is 23 in
1.5.6's own name table, the same value as 2.x, so the constant carries over.

Both 1.x hooks are on the game's hot path, so both early-out on
`dx12_streamline_ui_overlay::IsFrameTagTrackingActive()` (new, the same atomic `OnFrameTag` reads
first) plus a `g_hasPending` flag: while the bootstrap is dormant the steady state costs two atomic
loads and no `VirtualQuery`, `QueryInterface` or held reference. The remembered texture is adopted
from the `QueryInterface` that proved it and released by the very next evaluate.

**`streamline_dll_path` cannot upgrade a 1.x title to 2.x, and now says so.** A 1.x game imports the
five 1.x-only exports above; a 2.x interposer exports none of them, so the loader kills the process
before its first frame, and the names that do survive carry the truncating signature mismatch.
`StreamlineOverrideGenerationMatches` compares the replacement file's `VS_FIXEDFILEINFO` major
against the loaded interposer's - the one property the whole sl.* set shares, plugins included - and
refuses a mismatch on both redirect routes with a log that names the generation-independent
alternative (`dlss_sr_dll_path` / `dlss_fg_dll_path`, the NGX runtimes). Unknown on either side keeps
the historical behavior rather than breaking a working setup on an unreadable version resource.

### 2026-08-20 - The Witcher 3 does not start because the NVIDIA driver refuses that executable name

Not a CE bug, and provable in two seconds. Sessions `20260820_211008` (CE injected) and the game's own
`%LOCALAPPDATA%\REDEngine\ReportQueue\Witcher3_20260820_184315322` (CE **not** injected - no
`capture_hook_x64` in the module list) hold the *identical* failure: an unhandled C++ exception
`0xE06D7363`, throwinfo `witcher3+0x311e8d8`, message `... HRESULT of 0x887A0004`, exception object
carrying `DXGI_ERROR_UNSUPPORTED` at `+0x10`.

**What the game does** (disassembled from `witcher3.exe` 4.0.103190, function at `+0x7f7b40`): create a
factory, walk `IDXGIFactory1::EnumAdapters1`, skip any adapter with `DXGI_ADAPTER_FLAG_SOFTWARE`, and probe
each remaining one with `D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, nullptr)`
(`+0x7f7bf0`). The first that answers wins. If none does, the selected-adapter slot stays null and the final
`D3D12CreateDevice(nullptr, 11_0, ..., &device)` at `+0x7f7c3f` throws on failure. Both calls go through
`sl.interposer.dll!D3D12CreateDevice`. The "Microsoft Basic Render Driver" description sitting in the
throwing frame is the last *skipped* adapter's leftover `DXGI_ADAPTER_DESC1`, not the failing one.

**Root cause.** The NVIDIA driver (32.0.16.1088) refuses to create any hardware D3D device for a process
whose executable is named `witcher3.exe`. A byte-identical probe binary in the same folder:

| name | `D3D12CreateDevice(default, 11_0)` | `D3D11CreateDevice(hardware)` |
| --- | --- | --- |
| `witcher3.exe` / `Witcher3.exe`, any directory | `0x887A0004` | `0x887A0004` |
| `witcher3x.exe`, `Cyberpunk2077.exe`, `RDR2.exe`, `GTA5_Enhanced.exe`, anything else | `S_OK` | `S_OK` (FL 11_1, RTX 5070) |

DXGI still enumerates and describes the RTX 5070 correctly in the refused process; only device creation
fails, and it fails for **both** runtimes, which puts the refusal below D3D11/D3D12 in the display driver.
`nvldumdx.dll` maps and unmaps once per attempt and `nvwgf2umx` is never reached. Setting name matching is
case-insensitive and path-independent. NvAPI DRS enumeration (7893 profiles) finds the predefined profile
`The Witcher 3` owning `witcher3.exe` with all 34 settings at their predefined values and **no** user
profile for it anywhere, so this is the driver's own shipped per-application behavior, not a user override.
The Witcher-3-only settings in that profile are `0x0043C7A2`, `0x00CC0F79`, `0x1085DA8A`, `0x10AD7F3B`,
`0x10D5C2DB` plus a block of opaque `0x70xxxxxx` per-game driver workarounds; which one does it is not
observable from outside the driver.

**Consequences for CE.** CE's own `D3D12CreateDevice(nullptr, FL 11_0)` in the temp-swapchain bootstrap was
refused the same way and logged `hr=0x887A0004` thirty times in five seconds - each attempt a real ~48 ms
driver load. Two changes came out of this:

- `hook/common/d3d12_device_creation_policy.h` + `hook/apis/dx12_device_creation_report.cpp`: a failing
  device creation now emits one report - entry-byte integrity and jump-target owner for
  `D3D12CreateDevice` / `D3D12GetInterface` / `D3D12EnableExperimentalFeatures` / `CreateDXGIFactory1` /
  `CreateDXGIFactory2`, the loaded `D3D12Core.dll` and any declared Agility SDK, the non-Windows modules in
  the process, an adapter x feature-level matrix probed with a null `ppDevice` (so it creates nothing), the
  same call repeated past a foreign entry patch when one exists, and a verdict. `DisplayDriverRefusesThisProcess`
  is the verdict this session earned: DXGI describes the hardware adapter but D3D11 *and* D3D12 both fail on
  it. The D3D11 cross-check only runs once D3D12 has already refused every hardware adapter.
- Terminal creation failures (`DXGI_ERROR_UNSUPPORTED`, `DXGI_ERROR_SDK_COMPONENT_MISSING`, `E_NOINTERFACE`,
  `E_INVALIDARG`) are retried three times and then abandoned. They do not become success later in the
  process, and the retry storm was pure cost inside the game's startup.

**Ruled out along the way,** all by standalone reproduction on the same machine: RTSS (`RTSSHooks64.dll` is
in every failing run, but loading it into a clean probe changes nothing and patches no export),
`sl.interposer.dll`, the game's Agility SDK 1.600 in `bind_dx12\D3D12\` (declared but silently declined -
the OS `C:\Windows\System32\D3D12Core.dll` 10.0.26100.8972 is what loads, in the probe and in the game
alike), `D3D12EnableExperimentalFeatures`, a forced debug layer (no such registry state), and game-file
tampering (every DLL in `bind_dx12` carries a valid CD PROJEKT / NVIDIA / Intel / Microsoft signature).
WARP is the only adapter on this machine that fails anything in a healthy process, and only at FL 12_2 -
which the game never asks for, because it skips software adapters.

**Fixes available to the user,** none of them CE's: roll back or clean-install the NVIDIA driver; move
`witcher3.exe` into a *user* DRS profile so the predefined one stops applying; or rename the executable.

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
