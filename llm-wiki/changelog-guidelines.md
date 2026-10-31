# Changelog and Release Notes Guidelines

Last cross-checked: 2026-10-31 (reconciled the missed v0.1.7158 promotion against its published notes and source tag; preserved later changes in Unreleased).

Primary sources:
- `AGENTS.md`
- `CHANGELOG.md`
- `tools/manage_changelog.py`
- `tools/tests/test_changelog.py`
- `.github/workflows/release-stable.yml`

## Purpose

This document establishes the mandatory standards and workflows for maintaining `CHANGELOG.md` and generating GitHub release tag notes. Changes must be transparent, continuously updated, and ADHD-friendly so both users and developers instantly grasp what real-world issue or behavior was improved or corrected.

## Core Principles

### 1. Transparent Application Issue Attribution
- **Every entry must be transparent about the application issue it solves.** It must state what observable behavior, bug, game crash, stutter, artifact, or operational capability was fixed or added.
- **Do not write purely internal implementation notes** without explaining their application effect.
  - *Bad:* `- **AtomicSharedOwner:** improved recursive lock logic.`
  - *Good:* `- **Crash during recording finalization:** fixed deadlock when stopping capture while privacy blackout checks are active.`
  - *Bad:* `- **D3D12 hook:** updated fence drain routine.`
  - *Good:* `- **DirectX 12 overlay shutdown:** fixed crash during game exit when draining graphics command queues.`
- **Explicitly name affected titles, APIs, or subsystems** when a fix targets specific behavior (e.g. `The Witcher 3 (DX11)`, `Portal RTX`, `Gothic II (DirectDraw)`, `DOOM Eternal (Vulkan)`, `WGC capture`).

### 2. ADHD-Friendly and Highly Scannable Structure
- **Bold anchor summary on every bullet:** Every entry begins with `- **<Anchor>:**` or `- **<Anchor>**`.
  A reader skimming only the bold text across a release section must be able to understand all notable changes in under 15 seconds.
- **Punchy, direct sentence structure:** State the symptom or capability first, followed by the concise mechanism and consequence.
- **Standard Keep a Changelog categories:**
  - `### New`: Newly added features, settings, or supported titles/APIs.
  - `### Improved`: Latency reductions, performance wins, UX/overlay enhancements, and robustness improvements.
  - `### Fixed`: Bug fixes, crash resolutions, freeze eliminations, and rendering corrections.
  - `### Changed`: Intentional behavioral changes or setting defaults.
  - `### Removed`: Deprecated or removed capabilities.
  - `### Security`: Hardening or vulnerability remediations.

### 3. Continuous Incremental Updates in the Development Loop
- **Agents must update `CHANGELOG.md` before committing code changes.**
- Never defer changelog updates to batch release-prep sessions. Allowing `CHANGELOG.md` to lag behind git history results in forgotten fixes and lost context.
- Keep `## Unreleased` at the top of `CHANGELOG.md` current with all completed work since the last stable release tag.

### 4. Parity Between CHANGELOG.md and GitHub Release Tag Notes
- **GitHub release tag notes must meet these identical criteria.** Release notes published to GitHub must not be generic boilerplate; they must feature the transparent, ADHD-friendly categorized bullet points for that release.
- Release notes are generated directly from the changelog section via `tools/manage_changelog.py --generate-release-notes` during `.github/workflows/release-stable.yml`, ensuring complete parity and zero drift between git tags and GitHub Releases.

### 5. Native Feature Framing (No Unnecessary 3rd-Party Tool References)
- **Do not unnecessarily cite, name, or claim parity with third-party tools or utilities** (e.g. NVIDIA Profile Inspector, Special K, RTSS, OBS, ReShade).
- **Frame features and controls natively:** describe the underlying capability, operating mode, API hook, or configuration setting provided by CaptureEngine itself rather than referencing third-party software.
- **Exceptions:** Mention third-party software *only* when resolving concrete, verified interoperability defects or compatibility behaviors with other tools running simultaneously (e.g. hook chaining or layering with the Steam overlay or RTSS).

## Entry Structure & Style Guide

```markdown
# Changelog

## Unreleased

Changes since [v0.1.6652](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.6652).

### New

- **FidelityFX CAS/RCAS post-processing sharpen:** added contrast-adaptive sharpening
  on D3D11, D3D12, and Vulkan via `sharpen=cas`/`rcas` and `sharpen_intensity`. Applies
  cleanly to both base and frame-generation presentations without filtering the overlay.

### Improved

- **Vulkan layer registration decoupled:** the Vulkan implicit layer manifest is now
  staged dynamically and cleaned up gracefully, preventing stale manifest paths.

### Fixed

- **Apartment teardown crash:** fixed crash caused by caching an unmarshaled COM interface
  pointer across thread apartment cleanup during virtual desktop focus changes.
```

## Automation & Tooling

The Python utility `tools/manage_changelog.py` provides automated verification, extraction, and release operations:

| Command | Purpose |
| --- | --- |
| `python tools/manage_changelog.py --validate` | Validates `CHANGELOG.md` structure, headers, categories, and bold anchor syntax. Fails if format is violated. |
| `python tools/manage_changelog.py --check-worktree` | Checks git worktree: returns non-zero if code files (`.cpp`, `.h`, `.py`) are modified without `CHANGELOG.md` being updated. |
| `python tools/manage_changelog.py --extract-notes <version>` | Extracts Markdown release notes for `<version>` (or `Unreleased`). |
| `python tools/manage_changelog.py --generate-release-notes --version <v> [--commit <sha>] [--output <path>]` | Generates full GitHub release notes with changelog highlights, asset breakdown, and license notices. Used directly by `release-stable.yml`. |
| `python tools/manage_changelog.py --promote-release <v> [--prev-tag <tag>]` | Promotes `## Unreleased` to `## v<v>`, links the previous release, and resets `## Unreleased`. `--promote-release --version <v>` is accepted too. |

### Where promotion fits in a release

The build number is a local counter (`tools/build/build_io.py:bump_and_write_build_version`),
not a git-derived one. `release-stable.yml` resolves the requested version (or the next release tag)
and seeds that counter before building, then verifies the product version before publication.
`## Unreleased` stays un-promoted until the release publishes successfully, and
`--generate-release-notes` falling back to `## Unreleased` is the normal path, not a degraded one.

Promotion is the operator's step **after** the release publishes; the workflow prints the exact
command in its job summary. Skipping it leaves shipped changes under `## Unreleased`, where later
development can mix with them. The baseline guard only refuses a fallback to Unreleased when its
`Changes since [v<that tag>]` line already names the requested release; it does not detect a missed
promotion while that line still names an older release.

Before reviewing Unreleased or preparing another release, check the latest published GitHub release
and confirm that it has its own changelog section and is the Unreleased baseline. If promotion was
missed and later changes already exist, use the released source tag's changelog and the published
release notes to identify the shipped entries. Move only those entries into the release section;
retain subsequent changes in Unreleased. Keep corrections already published in the release notes
with that release. Running `--promote-release` on the whole current Unreleased section at this point
would incorrectly include the later work. Verify that the released section matches the published
notes, no entries were lost or duplicated, and the historical sections remain unchanged.

Promotion inserts extracted notes through a callable regex replacement. A plain replacement string
interprets Windows paths such as `C:\Games\Steam` and sequences such as `\1` as regex escapes or
backreferences, so it can fail after successful publication or alter the notes. The
`test_promotion_preserves_literal_backslashes_in_notes` regression covers both forms and UNC paths.

## Invariants & Guardrails

- `CHANGELOG.md` must always begin with `# Changelog`.
- `## Unreleased` is mandatory and must precede tagged release sections.
- Every bullet item under `## Unreleased` and recent releases must begin with a non-empty bold lead (`- **Anchor**`).
- Category headings must belong strictly to the approved set (`New`, `Improved`, `Fixed`, `Changed`, `Deprecated`, `Removed`, `Security`).
- `tools/tests/test_changelog.py` validates both parser logic and the live `CHANGELOG.md` on every self-test / verification run.

## Open Questions / Stale-Risk

- Low risk: Format is standardized and backed by unit tests and automated CI release generation.
- Manual post-publication promotion remains a workflow risk; format validation cannot detect a missed promotion.
