# llm-wiki Log

### 2026-10-10 - Crash dumps capture the fault neighborhood

- Witcher 3 session 20261010_160937 died of its own compiled-in int3 trap (witcher3.exe + 0x1F10AC3,
  byte-identical to the on-disk binary, PE timestamp matching; the game's crashreporter ran). CE's
  pre-termination dump proved all of that but could not say what the game was reporting: the reporting
  thunk's LEA targets (.rdata strings/records) and the call-site code on the stack were not captured,
  while 85% of the 182 MB was module .data segments that answered nothing.
- `FaultNeighborhoodCollector` (captureengine/diagnostics) now feeds dbghelp's memory callback the
  faulting thread's stack, code windows around every code pointer on it, register windows, and the
  data its captured code references (relative CALL targets and rip-relative LEA/MOV targets; heuristic
  scan in `common/crash/fault_neighborhood_policy.h`, same mechanism and budgeting as the WoW64
  stacks, chained behind one callback, x64 only). CaptureEngine's own modules keep their writable
  sections in the dump.
- Crash-like pre-termination exits now take the fatal-assert dump scope (handles, stack-referenced
  memory, memory map, no module data segments); suspicious but clean exits keep the rich scope.
- `tools/dump_triage.py` automates the triage that session needed by hand: fault module/RVA and trap
  bytes, call-chain code pointers, harvested references with captured strings or a MISSING marker.
  `--self-test` runs in the Python tool self-tests; run against the real session dump it reproduces
  the manual findings and names the missing report payload.
- Also fixed the clang-tidy ratchet findings from the startup-import work (exception escape at
  std::thread entries and main, one narrowing conversion). The elevation service client worker now
  confines failures to its client instead of terminating the service process; the ratchet folded the
  lowered counts in. Lint green.
