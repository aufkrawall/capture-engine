# llm-wiki Log — Archive 2026-W19e

### 2026-05-04 — Add query-based prerender limit and bind-time AF override to wrapper D3D11 path

- **Motivation**: BioShock Infinite (x86 D3D11) uses the wrapper/IAT architecture.
  The query-based prerender limit (`ApplyPrerenderLimit` with D3D11_QUERY_EVENT ring buffer)
  and bind-time AF override (`SetSamplersWithOverrides11`) only existed in the vtable
  hook path (`DetourDX11Present`, vtable hooks on CreateSamplerState/PSSetSamplers etc.),
  which is never activated for wrapper-architecture games. The wrapper only had
  `ApplyPresentFrameLatencyOverrides` (SetMaximumFrameLatency, which fails on swapchains
  without IDXGISwapChain2 support) and create-time AF disable/mip-bias in
  `CWrapD3D11Device::CreateSamplerState` (which doesn't enable AF because it can't inspect
  SRVs at create time).
- **Changes**:
  1. **Expose `ApplyPrerenderLimit`** (`hook/apis/dx11_hook.h`): Removed `static` from
     `ApplyPrerenderLimit` in `dx11_hook.cpp`, declared it in `dx11_hook.h`. The wrapper
     Present calls it directly.
  2. **Wrapper prerender limit** (`hook/wrappers/dxgi_swapchain_wrap.cpp:1008-1023`):
     Added query-based prerender limit call in `CWrapDXGISwapChain::Present` after the
     existing `ApplyPresentFrameLatencyOverrides` call. Uses the same
     `D3D11_QUERY_EVENT` ring buffer from `dx11_hook.cpp`. Rate-limited debug logging
     with `g_WrapperPrerenderWaits` counter.
  3. **Wrapper bind-time AF override** (`hook/wrappers/d3d11_devicecontext_wrap.cpp`):
     Replaced the pass-through `PSSetSamplers`/`VSSetSamplers`/etc. with bind-time
     replacement sampler logic. Gets the real device, creates replacement samplers
     with AF enabled (gated by `WrapperSamplerAllowsForcedAF` which checks for no mips,
     border address, reduction filter, comparison func). Includes a replacement sampler
     cache with config-hash invalidation. Rate-limited debug logging with
     `g_WrapperAFApplied`, `g_WrapperAFSkip*`, `g_WrapperAFReplaced` counters.
  4. **Wrapper create-time AF** (`hook/wrappers/d3d11_device_wrap.cpp`): Already had
     `ApplySamplerOverrides` at create-time (AF off + mip bias), but it was missing
     the mip-bias logging. The bind-time path now handles AF enablement.
- **Files changed**: `hook/apis/dx11_hook.h`, `hook/apis/dx11_hook.cpp`,
  `hook/wrappers/d3d11_devicecontext_wrap.cpp`, `hook/wrappers/dxgi_swapchain_wrap.cpp`,
  `llm-wiki/log/recent.md`
- **Verification**: Build `0.1.2781`: `success=1`, all unit tests passed.
- **Stale risk**: Historical only. Superseded on 2026-05-05: the wrapper path now tracks
  SRVs and logical original samplers before reconciling forced AF, matching the vtable
  path's resource-aware model.

### 2026-05-04 — Fix DX9 false detection in DX11 games; add D3D11 AF and prerender debug logging

- **Motivation**: BioShock Infinite (DX11) showed misleading DX9 hook messages and no debug
  visibility for anisotropic filtering and prerender limit overrides.
- **DX9 false detection fix** (`hook/apis/dx9_hook.cpp:6399-6406`): Added `d3d11.dll` to
  the DX9 hook `skipReason` check. When d3d11.dll is loaded, DX9 hooks are now skipped
  with reason `"d3d11.dll (DX11 game)"` instead of proceeding to create a dummy D3D9 device.
- **DX9 hook creation skip** (`hook/main.cpp:1529`): Added `!dx11DllLoaded` guard to prevent
  creating the DX9Hook object at all when d3d11.dll is present. The skip log now includes
  `dx11Loaded` field.
- **D3D11 AF debug logging** (`hook/apis/dx11_hook.cpp`):
  - `SamplerAllowsForcedAF()`: Rate-limited logs for each skip reason (no mips, border
    address, reduction filter, comparison func) with `g_DiagSamplerSkip*` counters.
  - `ShouldForceAnisotropyForStageSlot()`: Rate-limited logs for slot>=8, no SRV bound,
    unsupported format, single-mip texture skips.
  - `ApplySamplerOverrides11()`: Rate-limited logs on AF apply (filter + anisotropy values,
    up to 48), mip override (tri/bilinear), mip bias changes (up to 24), SGSSAA and Unity
    clamp events.
  - `GetOrCreateReplacementSampler11()`: Rate-limited logs on creation success (filter,
    anisotropy, bias, stage, slot — up to 48) and failure.
  - `DetourCreateSamplerState()`: Now logs on successful modification (not just failure).
- **D3D11 prerender debug logging** (`hook/apis/dx11_hook.cpp`):
  - `ApplyPrerenderLimit()`: Logs ring buffer creation with limit value, serial/buffered
    wait times in microseconds (up to 12), fractional idle gap calculation (up to 6),
    GetDevice failure.
  - `g_DiagPrerenderFrames` and `g_DiagPrerenderWaits` counters.
- **Diagnostic summary** (`DX11Hook::Shutdown()` and `OnHostDisconnect()`): Logs aggregated
  counters for all AF skip reasons, AF applies, replacements, mip bias/override events,
  prerender frames and waits in a single compact line.
- **Files changed**: `hook/apis/dx11_hook.cpp`, `hook/apis/dx9_hook.cpp`, `hook/main.cpp`,
  `llm-wiki/log/recent.md`
- **Verification**: Build `0.1.2778`: `success=1`, all unit tests passed.
- **Stale risk**: Low. The DX9 skipReason change only affects DX11 games that load d3d9.dll
  as a transitive dependency (UE3, Unity). The debug logging is rate-limited and won't flood
  logs. The diagnostic summary provides a single-line overview on shutdown.
