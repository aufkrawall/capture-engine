# llm-wiki Log Archive

### 2026-10-12 - Stop failed inject output and bind audio-worker lifetimes

- Session 0.1.7109: first capture finalized normally; the second lost NVENC's device alongside
  NVIDIA GPU-exception events. CE's sustained-failure stop entered an impossible CFR drain,
  leaving an old worker overwriting the next start's shared phase and a 9.97 GB repeated-error log.
- The production iteration runner now prioritizes terminal output failure over retry/debt;
  scheduled and catch-up output all reach failure monitoring. Healthy retries remain supported.
- Removed shared fences never become successful completion or GPU waits; synchronization errors
  propagate, and allocation/wait failure diagnostics carry bounded repeat counts and device evidence.
- Audio workers receive non-inherited kill-on-close jobs atomically at restricted process creation.
  Fifteen abandoned workers with a dead media parent were confirmed and stopped; zero remain.
- Production source safeguards fail against the old sources and pass after byte-exact restoration.
  Real Windows child tests verify atomic job membership, owner-loss cleanup, normal exit and refusal.
- Final unfiltered closing gate passed in 319.8 s: native tests, 41 Python tool groups, all 61 WARP
  FG flows, PE/import/PDB privacy checks and setup 0.1.7111 (39,522,516 bytes). Evidence:
  `build/verification/20261012_020023_build_7111`. Live GPU-fault/game retest remains unavailable;
  the underlying GPU exception's cause is not established. No optional lint/sanitizer/fuzz gate ran.

### 2026-10-12 - Give translated frontend decisions their own observed COM lifetime

- Native private data now carries output evidence only; each frontend decision has an exact
  Release predecessor, in-flight invalidation and final-release retirement without COM retention.
- All four cache regressions fail as assertions with the old owner source and pass after restoration;
  they cover address reuse, reuse before final Release returns and independent frontend decisions.
- Nested Present/Present1 retains two initial device queries per layer and six capture publications;
  actual output has two ownership logs for six frames. Borrowed-IUnknown fallback logs only once.
- Unfiltered closing gate passed in 313.0 s: both hook architectures, the full native suite,
  41 Python tool groups, all 61 WARP FG flows and PE/import/privacy checks. Evidence:
  `build/verification/20261012_001340_build_7109`; setup 0.1.7109 is 39,517,610 bytes.
- Live DXUP retest is unavailable: the recorded game folder no longer has dxgi.dll/d3d11.dll;
  the bounded local probe exited before loading CE. No lint, sanitizer or fuzz gate was run.
