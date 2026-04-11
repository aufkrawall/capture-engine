# Code Style

Last cross-checked: 2026-04-11

Primary sources:
- `.clang-format`
- `.flake8`
- `pyrightconfig.json`
- `common/raii_helpers.h`
- representative headers such as `common/shared_defs.h`, `hook/common/dx12_overlay_policy.h`, and `hook/wrappers/custom_hook.h`

## Scope
This page records the style rules that are either tool-backed or strongly reflected in the current tree. Local file conventions still win if a touched subsystem clearly uses a different established pattern.

## Tool-Backed Rules

### C++
- `clang-format` is based on Google style.
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
- Formatter config is high confidence and low stale risk.
- Naming and local-pattern guidance is medium confidence and should be re-checked against the files you touch.
- If formatter output, local file style, and this page disagree, prefer formatter output plus the local subsystem's established pattern.
