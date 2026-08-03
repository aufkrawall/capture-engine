# Code Style

Last cross-checked: 2026-07-28

Primary sources:
- `AGENTS.md`
- `.clang-format`
- `.flake8`
- `pyrightconfig.json`
- `common/raii_helpers.h`
- representative headers such as `common/shared_defs.h`, `hook/common/dx12_overlay_policy.h`, and `hook/wrappers/custom_hook.h`

## Scope
This page records the style rules that are either tool-backed or strongly reflected in the current tree. Local file conventions still win if a touched subsystem clearly uses a different established pattern.

## Tool-Backed Rules

### C++
- `.clang-format` is based on Google style, but it is descriptive guidance for existing files rather than permission to rewrite them wholesale.
- Column limit is 120.
- Indent width is 4 spaces and tabs are disabled.
- Brace style is attached / K&R.
- Pointer and reference alignment is on the left.
- Include blocks are preserved.
- Include sorting is case-sensitive.
- `Standard: c++20`.

### Python
- `black` is run with `--line-length 120`.
- `flake8` max line length is 120.
- `flake8` ignores `E203`.
- `pyright` uses `typeCheckingMode: basic`.
- `pyright` targets Python 3.10 on Windows.
- `pyright` has `analyzeUnannotatedFunctions: true`.

### Bounded source fragments

- The governed source ceiling is 800 lines; split toward a working target near 650-750 and never pad a small file.
- C++ `.inl` fragments are ordered includes inside the original `.cpp` translation unit. Keep anonymous/file-static state,
  preprocessor groups, strict-FP source identity, hot paths, and explicit build lists intact.
- Python compatibility facades execute ordered fragments in one module-global namespace. This preserves direct scripts,
  `import build`, monkeypatching, constants, and CLI behavior. `*_part_*.py` files are excluded from standalone flake8/
  pyright discovery because their names/imports intentionally come from neighboring fragments; source reassembly tests,
  `compileall`, facade execution, and the full verification gate cover the logical program.
- Before accepting a split, compare logical source reassembly, inspect symbol/state ownership, run focused tests, and use
  `--verify` for build, parser/configuration, media/capture, audio, graphics, or FG changes. Keep
  `tools/file_size_baseline.json` at zero rather than treating it as a permanent allowlist.

## Common Tree Conventions
- Headers generally use `#pragma once`.
- Standard and platform headers use angle brackets. Project headers use quotes.
- Types and most C++ functions and methods are usually `PascalCase`.
- Local variables and parameters are usually `camelCase`.
- Python functions and variables are usually `snake_case`.
- Globals often use a `g_` prefix.
- Many constants use `kName`. ABI and shared constants also use all-caps names such as `SHARED_MEMORY_VERSION`.
- Fixed-width integers are common in shared-memory, hook, and ABI-sensitive code.
- `enum class` is common when introducing new enums.
- Match the touched file's local naming and member-style convention instead of renaming neighbors.

## Existing Helper Patterns
- Common RAII helpers already live in `common/raii_helpers.h`: `HandleGuard`, `MappingGuard`, `VirtualAllocGuard`, `ComGuard`, and `ScopeGuard`.
- Hooking code is not one-size-fits-all in this tree. Current subsystems use wrapper hooks plus typed hook helpers such as `HookSystem::TypedHook` and `CustomHook::TypedHook`. Match the local subsystem pattern instead of forcing a global hook abstraction rewrite.

## Practical Notes
- Do not run `clang-format.exe -i`, `python build.py --format`, or another whole-file automatic formatter on existing source unless explicitly requested. The current tree contains legacy formatting and mixed stored line endings; an in-place whole-file pass can create large unrelated diffs even when the intended edit is narrow.
- Preserve the touched file's existing formatting and line endings. Inspect `git diff --check` and the semantic `git diff` before building.
- Perform the required full build once after the final code change set, not after every intermediate edit. Stage only task-owned paths; use `git add -A` only when every worktree change has been verified task-owned and safe.
- Scope dump analysis, LSP cleanup, and wiki edits to the task/touched area. Update the wiki only for durable future-useful knowledge, and keep diagnostic logging high-signal and rate-limited.
- Formatter configuration values are high confidence as style guidance, but automatic whole-file conformance is intentionally not assumed.
- Naming and local-pattern guidance is medium confidence and should be re-checked against the files you touch.
- If formatter output, local file style, and this page disagree, preserve the local subsystem's established pattern unless the user explicitly requested a formatting migration.

### Current clang-tidy debt and triage

As of 2026-08-03 the clang-tidy baseline is **zero warnings** across 272 translation
units. Remaining analyzer findings are either fixed at the root or carry a targeted
`NOLINT` comment with a concrete rationale; checks are never disabled globally. See
`known-debt.md` for the category-by-category disposition.

`run_tests()` captures the executable's stdout/stderr and, on failure, writes `unit_tests_failure.log` into the verification bundle while logging the diagnostic tail. This is required because an exit code without the failing GoogleTest name is not actionable.
