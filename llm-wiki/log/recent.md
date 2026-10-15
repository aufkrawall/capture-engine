# llm-wiki Log

### 2026-10-15 - Classic D3D9 multithreaded unlock: decoded device lock, review fixes

- Review of `49a3296e` found the device lock entered at an assumed `device+sizeof(void*)` without validation and
  on `D3DCREATE_MULTITHREADED` alone (also when `GetCreationParameters` failed). Disassembly (public symbols,
  d3d9.dll 10.0.26100) shows `CLockD3D` enters the section only when the gate dword past it is nonzero: x86
  `[device+4]+0x18`, x64 `[device+8]+0x28`. Now decoded from `GetDirect3D` and proven on CE's multithreaded Ex
  helper; the target's gate must match its flags; no proven lock means multithreaded game devices decline.
- The flag-write branch was dead (the reference factory is always known with the layout) and was removed; the swap
  is an atomic compare-exchange. The swap also redirects `CheckDeviceFormat` and per-adapter table reads in
  `CMipMap::Create`/`CDriverSurface` to CE's factory, so both factories must agree on the device's adapter.
- `GetDirect3D` and `QueryInterface` take `CLockD3D`, so concurrent callers on a multithreaded device block.
- Hardware run pending for both the swap and multithreaded devices.

### 2026-10-15 - Classic D3D9 zero-copy unlocked for multithreaded devices

- Disassembly of d3d9.dll confirmed that multiple internal methods (`GetDesc`, `GetPriority`, `ClearI`, `UpdateSurface`,
  and resource creation) check the factory's Ex flag, and that multithreaded devices serialize through `CLockD3D`
  on `m_critSec` at `device + sizeof(void*)` (superseded above: the offset is now decoded and gated).
- Serialized `DX9ClassicShareUnlockScope` against the runtime's device lock and scoped the unlock by temporarily
  repointing the device's factory field (`device + factoryFieldOffset`) to CE's reference Ex factory (`g_exFactory`).
- The game's original factory is never modified, isolating other devices on the same factory.
- Enabled multithreaded devices in `DecideUnlock`; no hardware run.

### 2026-10-15 - DX9 shared-ring capture cost in perf metrics

- The perf CSV showed `capture_us=0` for Mirror's Edge because capture ran deferred in a nested EndScene, outside
  the Present window. Added a per-Present cost accumulator (submit, drain, deferred CaptureFrame) consumed every
  Present, mapped to `stretch_rect_us`/`query_wait_us`/`capture_us`, plus a 10 s summary line. Hardware run pending.
- Multithreaded classic devices: see the device-lock entry above.

### 2026-10-15 - Classic D3D9 Ex-gate unlock validated in Mirror's Edge

- Session `20261015_125417`: `D3D9-SHARED-DIRECT` with `classicUnlock=1`, 2235 live frames, healthy recording.
  The factory-mismatch of the first run was the Steam overlay wrapping `Direct3DCreate9Ex` factories
  (`vtbl=gameoverlayrenderer.dll`); the runtime-reported factory is the correct reference.

### 2026-10-15 - Mirror's Edge run: Ex-gate reference factory and GDI double cadence gate

- Session `20261015_123230` (x86, single-threaded classic device, Steam overlay): the Ex-gate layout decoded
  correctly but was rejected as `factory-mismatch` because validation compared against CE's own Ex factory handle.
  Now anchored on the runtime's `GetDirect3D` result with vtable-in-d3d9 checks and an owner-naming diagnostic.
- The fallback (GDI interop) published no frames: `CaptureFrame` re-consulted the cadence gate after the caller's
  admission. Fixed; no unit test (needs a D3D9 device), evidence is the skip-log signature above.
- Pending: rerun; expect `Classic share layout located` and `classicUnlock=1` for near-free capture.

### 2026-10-15 - Classic D3D9 zero-copy via the runtime's Ex gate

- Classic `Direct3DCreate9` devices are refused shared resources by d3d9.dll's own validation (factory "is Ex"
  BOOL), not by the driver. CE now locates that BOOL per process (decode slot 125 of its Ex helper, validate on
  Ex and classic objects, prove with an unlocked game-device probe) and sets it only around its own shared
  create/open calls. Single-threaded game devices only; see `d3d9-capture.md` "Classic Ex-gate unlock".
- Pending: user hardware run with a 32-bit classic D3D9 game (look for `Classic share layout located` and
  `classicUnlock=1`).

### 2026-10-15 - Side effects on non-whitelisted processes removed

- Audit of everything CE does to processes outside the whitelist. No injection reaches them (discovery opens
  only limited-query handles, non-whitelisted children of injected games are created byte-identically, the
  creator role is dormant), but three costs did:
  1. Process discovery leased the service's display trace, keeping DXGI/DxgKrnl/Win32k display-timing providers
     on machine-wide for the whole CE lifetime. Now a process-only lease; display providers follow display leases.
  2. `ngx_ota=off` host termination killed every `nvngx_update.exe` by name. Now only one whose parent is a
     whitelisted target that predates it.
  3. The Vulkan gate snapshotted all processes for every non-target Vulkan process while a host was published.
     Now parent PID first from the process's own record; the parent is opened only on a PID match.
- Kept by design: the resident gate maps into every Vulkan process (also `dll_injection=never`); the LL keyboard
  hook swallows CE's own hotkey combinations desktop-wide. Wiki claims of `never` being fully non-injected were
  qualified for Vulkan. Not validated on hardware yet: service lease switching and `Left NGX updater` lines.

### 2026-10-15 - Attested stable release v0.1.7153 and pre-release lint ratchet fixes

- Pre-release `--no-build --lint` caught clang-tidy regressions in newly added units: explicit `D3D12_HEAP_PROPERTIES`
  enum initialization in `hook/capture/inline_capture_completion.h` and `tests/flow/test_flow_inject_fg_switch.cpp`
  resolved `bugprone-invalid-enum-default-initialization`, `noexcept` specifications and caught handlers on `TraceConsumer`
  and `EtwMonitor` resolved `bugprone-exception-escape`, and explicit `static_cast<void*>` and `HLOCAL` pointer casts
  resolved `bugprone-multi-level-implicit-pointer-conversion` (tightening the baseline from 5 to 0).
- Unfiltered closing gate passed and minted setup 0.1.7153 (40,024,982 bytes).
- Dispatched `release-stable.yml` run `38652712702` (`version=0.1.7153`, `attest=always` on `2a43d535`). Built
  all 8 FFmpeg source dependencies and custom FFmpeg from source, passed full `--verify --verify-clean --portable-archives`
  (full native suite, Python tool self-tests, 78 FG flow scenarios, ASan/UBSan, clang-tidy ratchet, PE hardening and privacy scrub),
  attested all 6 release assets via Sigstore (`gh attestation verify` confirmed), published `v0.1.7153`, verified
  `release-log-cleanup.yml` (`38657268334`) purged the self-hosted run log, and promoted `CHANGELOG.md`.
