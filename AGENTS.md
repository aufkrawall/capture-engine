# CaptureEngine Agent Guide

Repository-specific instructions for coding agents working in this repo.

## Scope
- Root: `C:\Users\TestUser\Programme\build\captureproject`
- Platform: Windows-first (prefer Windows commands over Linux commands), PowerShell 7.6, MinGW/Clang via `build.py`
- Canonical derived documentation lives in `llm-wiki/`

## Error Handling
- Crash dump available? | Analyze with cdb.exe in `C:\Program Files\Windows Kits\10\Debuggers\x64`
- ffmpeg and ffprobe .exe location | When prompt asks to analyze media file `C:\Users\TestUser\Programme\build\captureproject\build\msys64\clang64\bin`
- HACKS LIKE COMPLETELY DISABLING OUR OVERLAY WITH FSR FG OR DLSS FG TO PREVENT CRASHES ARE UNACCEPTABLE!! FIND PROPER FIXES!!!
- WE DO NOT WANT TO USE D3D11ON12, WE WANT TO USE NATIVE DX12 FOR THE DX12 OVERLAY!
- WE ALREADY HAD OUR OVERLAY WORKING IN BOTH GTA V ENHANCED AND TALOS REAWAKENED WITH FSR FG, IT DEFINITELY IS POSSIBLE! THE SAME IS TRUE FOR DLSS FG!

## llm-wiki Workflow
- Always consult `llm-wiki/index.md` first, then the relevant `llm-wiki/*.md` pages, then `llm-wiki/log.md` for recent updates and stale-risk notes.
- `llm-wiki` must always be consulted.
- `llm-wiki` must constantly be cross-checked for factual correctness against the real sources of truth: implementation, tests, build scripts, config files, and current behavior.
- `llm-wiki`, code and code annotations must be mistrusted. They can all be correct or incorrect. If they contradict each other, we must think of a correct way to align all of them in a shared correct state.
- `llm-wiki` must constantly get updated to maintain the highest practical factual correctness and completeness.
- Do not conflate `llm-wiki` with the concrete project code.
- Our llm-wiki must be dynamically extended with new information, corrections and entries, for example when new realizations occur on new code changes.
- When you fixed a bug or implemented a feature, always check if our llm-wiki should be updated.
- This can also mean already existing llm-wiki entries and information might need corrections or other changes, perhaps even deletions, after new changes applied to the code base to maintain up-to-dateness.
- Update the llm-wiki instantly after having implemented even a partial code fix or code improvement, and also include mentionings the remaining parts which are yet to be implemented. That way we do not lose valuable information when context window compaction kicks in before we are done, unlike when we would update the llm-wiki only once after all steps are completed!

## Project Constraints
- We are paranoid about regressions. Add regression coverage where feasible when fixing bugs (add new test units etc.).
- We are also paranoid about adding sufficient debug logging to better support diagnosis and avoid making decisions from insufficient evidence. Always be on the look-out if debug logging can be improved to make fixing an issue easier.
- Overlay, injection, and frame-generation fixes must stay generic. Do not add game-specific compatibility hacks.
- Do not disable features as a workaround.
- Do not use sleeps, wait tables, or other timing bandaids as a crash fix or race workaround.
- Do not introduce racy or otherwise frail behavior.
- Prefer thorough, maintainable, hardened fixes over quick patches.
- Switching between FG modes must work gracefully in Talos and GTA validation scenarios, in all directions and combinations, without crashes, without lost overlay rendering, and with the correct visible FG status.

## Practical Agent Rules
- After code changes, rebuild with `python build.py --skip-updates`.
- Do not sit in long passive watch loops waiting for build/test completion. Re-check `build/verification/latest_summary.txt` or `latest_manifest.json` directly instead; those files are the completion/status contract.
- DO NOT RUN `python build.py --version` !!
- Match the local subsystem pattern instead of imposing a new one.
- The worktree may already contain unrelated user edits; do not revert them.
- Don't wait ages on truncated outputs, check results directly without wasting time.
- Generally, commit changes, unless explicitly told otherwise.
