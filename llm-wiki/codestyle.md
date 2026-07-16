# Code Style

Last cross-checked: 2026-07-12

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

The 2026-07-17 inventory ran against the existing `compile_commands.json` (217 C++ entries) with the project header boundary in `.clang-tidy`. It found 1,159 project findings: 541 in `hook`, 269 in `tests`, 215 in `testapp`, 82 in `mediaengine`, 43 in `captureengine`, and 9 in `common`. External, generated, installed, and FFmpeg trees are excluded and are not part of this debt.

Use these dispositions; they are triage buckets, not blanket suppressions:

| Disposition | Checks and counts | Required handling |
| --- | --- | --- |
| Correctness/design review (380) | `invalid-enum-default-initialization` 137; `throwing-static-initialization` 62; `unchecked-string-to-number-conversion` 42; `exception-escape` 26; `unchecked-optional-access` 26; `incorrect-roundings` 19; `inc-dec-in-conditions` 10; `switch-missing-default-case` 9; `nondeterministic-pointer-iteration-order` 8; `misplaced-widening-cast` 7; `suspicious-memory-comparison` 6; `signed-char-misuse` 4; `sizeof-expression` 4; `use-after-move` 10; `empty-catch` 3; `unused-return-value` 3; `integer-division` 2; `raw-memory-call-on-non-trivial-type` 1; `redundant-branch-condition` 1 | Inspect behavior and add focused regression coverage before changing or documenting the finding. Never globally disable these checks. |
| Conversion/ABI/intent review (762) | `narrowing-conversions` 337; `multi-level-implicit-pointer-conversion` 183; `argument-comment` 165; `bitwise-pointer-cast` 44; `branch-clone` 25; `macro-parentheses` 7; `casting-through-void` 1 | Verify range/ABI/lifetime assumptions. Use explicit local conversions, wrappers, or narrow documented suppressions only when the intent is proven. |
| Performance review (17) | `inefficient-vector-operation` 6; `unnecessary-value-param` 7; `no-automatic-move` 4 | Change only when ownership and measured behavior support it; do not trade hot-path correctness for warning count. |

`run_tests()` captures the executable's stdout/stderr and, on failure, writes `unit_tests_failure.log` into the verification bundle while logging the diagnostic tail. This is required because an exit code without the failing GoogleTest name is not actionable.
