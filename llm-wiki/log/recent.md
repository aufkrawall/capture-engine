# llm-wiki Log

### 2026-09-20 - Automated changelog handling and ADHD-friendly guidelines

Automated `CHANGELOG.md` validation, extraction, and GitHub release notes generation:
- **Pre-commit changelog rule in `AGENTS.md`:** Agents must update `## Unreleased` in `CHANGELOG.md` before
  committing code changes, preventing changelog drift.
- **`llm-wiki/changelog-guidelines.md`:** Established core principles:
  - Transparent application issue attribution: every entry must state the user-facing defect, crash, or
    capability solved rather than purely internal code mechanism.
  - ADHD-friendly scannability: bold lead-in anchor on every bullet (`- **<Anchor>** <Details>`), standard
    categories (`### New`, `### Improved`, `### Fixed`), short punchy sentences scannable in 15 seconds.
  - Continuous incremental updates during dev loops under `## Unreleased`.
  - Parity between `CHANGELOG.md` and GitHub release tag notes.
- **Automation tooling (`tools/manage_changelog.py`):** CLI and library for `--validate`, `--check-worktree`,
  `--extract-notes`, `--generate-release-notes`, and `--promote-release`. Validated continuously via
  `tools/tests/test_changelog.py` in `tools/python_tool_self_tests.py`.
- **GitHub Actions integration:** `.github/workflows/release-stable.yml` now generates release notes directly
  via `python tools/manage_changelog.py --generate-release-notes --version $version --commit $env:COMMIT --output $releaseNotesFile`
  and passes `--notes-file` to `gh release create`, ensuring published release tag notes feature full issue highlights.

### 2026-09-20 - [StartupPerf] was reading a different object than the one being written

Session `20260920_211603`, build 0.1.6746, Talos Reawakened, ~90 s, clean exit. The session itself was
healthy - DLSS MSFG 3x, overlay on the PostSL route at `render%=100%`, ~117 fps output over ~39 base,
1% low ~100, CE's own per-frame cost 18.6 us average, zero overload flags. One line did not fit:

```
[StartupPerf] Controller startup: VulkanRegistration=0.000 ms, ..., TotalToReady=36055.158 ms
```

The log timestamps put controller startup at `21:16:05.103` -> `.245`, i.e. **142 ms**, and the tray and
Vulkan registration visibly took ~11 ms and ~19 ms - so two measured fields read zero and the total was
254x too large.

**Root cause: an `inline` variable silently lost external linkage.** `main_internal.h` declared

```cpp
namespace { struct ControllerStartupTimingState { ... }; }
inline ControllerStartupTimingState main_g_ControllerStartupTiming;
```

An unnamed namespace gives the *type* internal linkage, and the variable inherits it. The `inline`
therefore merges nothing: every translation unit gets its own object. `main_entry.cpp` filled its copy,
`main_recording.cpp` read a zero-initialized one, and `TotalToReady` degenerated into
`Log_GetQpcUs() - 0` - an absolute QPC-since-boot reading, which is why it printed ~36 s (CE autostarts
at boot).

Confirmed empirically with the project's clang before touching the header: two TUs including one header,
writer sets 42, reader sees `0`; with the type moved out of the unnamed namespace, `42`. **No diagnostic
fires**, not under `-Wall -Wextra`, which is what made this survive.

Blast radius was diagnostics only. The `complete` re-entry guard is written and read inside
`main_recording.cpp`, so it stayed self-consistent and deferred startup never double-ran.

Fixed in `fb2afb1d` by moving the struct out of the unnamed namespace, with a comment saying why it must
stay out. Regression coverage is `tests/test_header_inline_variable_linkage.cpp`: one case anchored on
`main_g_ControllerStartupTiming`, plus a sweep of every first-party header (`common`, `hook`,
`captureengine`, `mediaengine`, `testapp`, `tests`) for an `inline` variable whose type comes from an
unnamed namespace in the same header. Both fail on the parent revision and pass on the fix; the sweep
found no other instance in ~475 headers.

Invariant worth keeping: **a type in an unnamed namespace in a header and an `inline` variable of that
type are mutually exclusive.** Either the type is shared, or the variable is not.

Two smaller things checked and cleared in the same pass, both correct as they stand:

- `[Screenshot] Output color policy: requested=both resolved=HDR AVIF plus BT.709 SDR PNG` followed by a
  single PNG is not a missed HDR write. The swapchain was `SDR-G22-P709`, and `SaveRawScreenshot`
  deliberately writes one PNG for an SDR source. The policy line just prints before the source is known,
  so it reads as a promise of two files. Cosmetic, unfixed.
- `Sharpen: DX12 running reason=cas param=0.000` is the configured `sharpen=cas/0.00/0.30/auto`: CAS at
  minimum sharpness with a 30% blend. `Decide` short-circuits on zero *intensity*, not zero strength, and
  CAS sharpness 0 is the effect's mildest setting rather than an identity pass.

`DisplayTiming` also logged `no screen-change timestamp published yet` at `21:20:42` with
`runtimePresents=0`; that is the loading screen before any present, and the next window published 1219.
Not a fault.
