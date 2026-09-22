# llm-wiki Log

### 2026-09-23 - Vulkan sharpen lost the device on DOOM Eternal's swapchain recreate

Session `20260922_235937`, build 0.1.6773, first run with `sharpen=cas` active at Vulkan
startup: the sharpen pass ran on the startup swapchain, DOOM destroyed it (no `oldSwapchain`)
and NVIDIA returned the same handle for the replacement. The sharpen state was never released
on destroy, passed the present-time generation check, and drew through freed image views:
`QueueSubmit FAILED with result -4`, black window. The manual dump shows only the game's
threads parked, with no CE frame, as expected for a GPU-side fault. This repeats both DOOM overlay
lifetime bugs (`20260913_174040`, `20260914_122133`) in a component added later. Fixed per
`post-processing-sharpen.md` "Vulkan swapchain lifetime". The same session's earlier DOOM run
shows the second bug: `sharpen=cas` published live twice, and the layer never read it. Hardware run
pending: start DOOM with sharpen on, toggle it live, change resolution/fullscreen.

Follow-up session `20260923_003755` (0.1.6774): the recreate was clean, then DOOM moved its present
to compute-only family 2 on the live swapchain and the render-pass route kept submitting there.
The device was lost 2.3 s later. Sharpen now has a compute route and rebuilds on a family change
(`post-processing-sharpen.md` "Vulkan compute route").

### 2026-09-22 - Nothing CE runs may make other applications' keystrokes wait

Review for "other applications register keyboard input delayed". The controller's
`WH_KEYBOARD_LL` hook (0.1.6181, `captureengine/hotkey_input_hook.cpp`) is the one CE thread
every keystroke on the desktop waits on, up to `LowLevelHooksTimeout` (300 ms default, 1 s cap).
Its callback path was mostly clean (Try-lock, `PostThreadMessage`), but:

- **It logged in the callback.** `LogDebug` per delivered hotkey and `LogError` on a failed
  post both take `g_LogMutex` and `fflush` every line - held by the controller exactly while a
  hotkey starts a recording, with the encoder writing to the same disk. Now the hook thread
  only counts; `ReportHotkeyInputHookDiagnostics()` (controller loop) logs deltas, and the
  delivery line is logged on receipt with the vk in `lParam`.
- **ABOVE_NORMAL (10) lost to game threads at 10-15** on a saturated CPU. Now TIME_CRITICAL.
- **Silent removal was invisible.** Windows 7+ removes an LL hook after repeated timeouts
  (the 11th). A callback older than half the timeout now posts a re-arm (fresh hook first,
  then unhook the old; a failing unhook proves the system removed it). A callback past the full
  timeout is bookkept but never consumed - the app already has that key, and consuming its
  key-down would leave the key-up unpaired.
- **A controller crash dump suspended the hook thread.** `RegisterCrashPreDumpCallback` (new,
  `common/crash_handler.h`) removes the hook before the dump worker starts.

**The limiter process was dead weight, and dangerous.** Since 91103513 (2026-05-05) the hook
paces in-process and never signals `CE_LQ_*`, so `captureengine/limiter_main.cpp` only waited -
at REALTIME class + TIME_CRITICAL + MMCSS Pro Audio critical, affinity hard-coded to `0x02`,
`timeBeginPeriod(1)`, with a QPC spin of up to ~2.5 ms per frame on the (unreachable) paced
path. Its `WaitForSingleObject(hRequestEvent, 100)` on a NULL handle (events not yet created)
returned WAIT_FAILED immediately: a priority-31 busy loop on core 1. It still gated
capture-synced recording ("limiter readiness failure"). Removed entirely: process, IPC mode
(value 3 left unassigned), controller spawn/recovery/shutdown, the shm fields
(`SHARED_MEMORY_VERSION` 62 -> 63), the hook's never-opened event handles and `hookSessionId`.

**The in-game WndProc subclass (`InputManager`) did nothing** but take a mutex per message
(every 8 kHz `WM_INPUT` included) and `HookLog` under it once per hook. Removed with its ten
call sites; no hook source may use `GWLP_WNDPROC` now. That also retires the "keep the WndProc
link resident during dejection" concern in `main_host_lifecycle.cpp`.

Pinned by `tests/test_desktop_input_latency.cpp` and two `ProcessIPCTest` cases (no
`SetThreadAffinityMask`, no self-applied realtime class anywhere in first-party sources).
**Runtime validation pending:** a session log should show `systemTimeout=... timeCritical=1` on
install and no `answered late` lines during normal play; `hkHook=` deliveries still work in
DOOM Eternal.

### 2026-09-21 - Stable release 0.1.6772, and the three gates only the release job runs

`v0.1.6757` was published at 14:39 and withdrawn to a draft after Strange Brigade broke; its
release and tag are deleted, so `v0.1.6652` -> `v0.1.6772` is the published line. Everything
6757 introduced reached users for the first time in 6772, so its CHANGELOG section is merged
into 6772's (which now covers changes since 6652) and the published notes were regenerated.
A withdrawn release means merging its section forward, not leaving it to document a version
nobody can download.

The first dispatch (`0.1.6767`, run 35629107044) failed 74 seconds in, inside the verification
preflight, and behind that one failure sat two more:

- `hook/common/hook_common.cpp` at 813 lines. Split into `hook_common_graphics_config.cpp`
  (485 + 339): the shared-memory/local config merge, the cached per-thread view, and the DLSS
  driver-settings values derived from it. The two source-policy tests asserting on those merge
  assignments now read the new unit. It deliberately does not include `fps_limiter.h` - that
  header pulls `reflex_limiter.h`, and a new translation unit re-emits its headers' warnings
  into the clang-tidy totals (+3 `unused-private-field` and more, purely from the include).
- `dxgi_shared_internal.h` longest line 228 -> 239, from `DetourResizeBuffers1ReconcileOnly`
  declared on one line in e07c3222.
- `clang-analyzer-core.CallAndMessage` 26 > 25 at `dxgi_shared_steam.cpp`'s guarded Steam
  Present. The .cpp had not changed since the baseline; e07c3222 changed a header it includes,
  which re-analyzed the unit. It is the documented false-positive class - every condition
  proving `externalPresent` is callable is evaluated into a bool and passed to a policy helper -
  so the call site now restates the guard, with a source-policy test holding it there.

**Rule.** `--no-build --lint` before dispatching a release, after a full product build. The
per-change gate does not lint, so a green build+test run proves nothing about the file-size,
line-length or clang-tidy ratchets the release job enforces. Run 35630917605 published
`v0.1.6772` from `25a7335b`; attestation verified against a downloaded `captureengine.7z`.

### 2026-09-21 - CE's own swapchain flag killed Strange Brigade's startup; DX12 sampler overrides reach nothing

Session `20260921_173511`, build 0.1.6757. The game showed
`Can't recover from driver error. Error Code 80070057`, exited with code 1, and never presented a
frame. The 49 MB `FREEZE` dump is CE's watchdog reacting to that modal box, not the event.

**Root cause.** `backbuffer_count` implements its depth by adding
`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` (0x40) to the *application's* creation
descriptor. `dxgi!CDXGISwapChain::ValidateResizeBuffers` XORs the caller's flags with the chain's
creation flags and returns `E_INVALIDARG` on any disagreement in that bit. Proof from the dump: the
chain's stored flags at `swapchain+0x184` were `0x842`, the game's own copy on its stack `0x802`.

The rewrite that hid the flag again lived in `CWrapDXGISwapChain` and in the ResizeBuffers vtable
detour. Steam owned the dxgi Present entry, so CE logged
`keeping the swapchain vtable pristine` and `Preserving real DX12 swapchain identity`, handed the
game the real swapchain, and installed neither. The mutation was unconditional, the compensation was
not - and it was duplicated across six creation paths, which is how they drifted apart.

Fixed in e07c3222: `hook/common/swapchain_flag_policy.h` holds the rule once, the reconciliation
reads the live `GetDesc().Flags` instead of re-deriving intent from the config (a stale "add the
bit" is as fatal as a missing one), a reconcile-only ResizeBuffers claim is installed at the DX12
and DX11 bootstrap independently of the Present-ownership question, and the flag is withheld when
no reconciliation can be established. Validated on hardware in `20260921_175749`:
`ResizeBuffers: Reconciling application resize flags 0x802 -> 0x842`, 10288 frames, clean exit.

**Second finding, from the same session.** Forced AF and `mip_bias=-3.0` did nothing: zero
`DX12 AF:` lines. `PatchIATAllModules("d3d12.dll", "D3D12CreateDevice", ...)` logs `patchResult=0`
because nothing imports it statically, and the injector waits for `d3d12.dll` to be *present* before
injecting (`waitMs=0`, `d3d12=1` on the first poll), so the game had already resolved the export.
Hooking the device CE discovers from the game's command queue (`DX12_PublishNativeLimiterDevice`)
was still too late: in `20260921_175749` the hooks came up at 17:58:01.068 and observed exactly two
static samplers all session, both CE's own overlay root signature.

All `ID3D12Device` objects share one D3D12Core vtable, exactly like `ID3D12CommandQueue` - which is
why `DX12_HookQueueVTable(pQueue)` on the bootstrap queue has always covered the game's pre-existing
queue. The device claim is now made on the WARP bootstrap device too. That claim was removed in
6323ed47 and guarded by a source test; the guarded rule is really "the WARP bootstrap must not become
*application evidence*", and a vtable claim is not that, so the test now asserts the claim exists and
that `MarkD3D12DeviceCreated` still does not. Confirmed on hardware 2026-09-21: forced AF and
`mip_bias` take effect in a DX12 title with the bootstrap vtable claim in place.

`LogSummary` also runs at frame 2000 now, not only at shutdown: "forced AF observed no sampler at
all" is useless information after the process is gone.
