# llm-wiki Log

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
