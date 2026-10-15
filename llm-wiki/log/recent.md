# llm-wiki Log

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
