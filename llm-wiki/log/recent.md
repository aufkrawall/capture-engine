# llm-wiki Log

### 2026-07-30 - Make the Linux cross build complete: portable thread handles, staged AMF license, host-independent path keys

- **Evidence / root cause:** with the test-app object collision fixed, the clean Linux build got further and exposed four more host-parity defects. (1) `mediaengine/video_encoder_part_005.inl` assigned `writerThread.native_handle()` straight to `HANDLE`; that only compiles where `std::thread::native_handle_type` *is* `HANDLE`, which is true for MSYS2 clang64's Win32 thread API and for Ubuntu CI's `update-alternatives` win32 default, but not for a winpthreads MinGW such as Arch's `mingw-w64-gcc` 16.1.0, where it is a `pthread_t`. Three further sites - `hook/common/system_metrics_part_001.inl`, `captureengine/injection_part_002.inl` (twice), `captureengine/media_main_part_002.inl` - used `reinterpret_cast<HANDLE>` instead, so they compiled everywhere and silently produced a bogus handle: a Wine reproduction of the old pattern returned `WAIT_FAILED` with `ERROR_INVALID_HANDLE` and handle `0x1`, meaning every bounded join in a cross-built binary took its failure path. (2) Packaging failed closed on a missing `amf-headers/LICENSE` because `mingw-w64-clang-x86_64-amf-headers` was in the Windows `PACKAGES` list but not in `LINUX_MSYS2_PACKAGES`; CI had never reached this stage. (3) `project_relative_key()` folded separators only after `os.path.relpath`, so a Windows-written compilation database read on Linux kept whole absolute paths as clang-tidy scope keys - and those keys go into the committed baseline. (4) `collect_link_dependency_paths()` searched extension-less linker names on Linux in the compiler's own directory, which finds the host ELF `/usr/bin/ld` rather than the cross linker the driver actually runs, so the link-cache signature tracked the wrong binary in both directions.
- **Fix / invariants:** `common/thread_wait.h` adds `ce::Win32ThreadHandle()`, which selects by exact `native_handle_type`: a non-template overload returns an already-Win32 handle unchanged, and a *template* overload unwraps a `pthread_t` through winpthreads' own `pthread_gethandle()`. Being a template, that body is instantiated only where it is needed, so toolchains whose winpthreads headers lack the accessor keep building - verified by compiling the HANDLE path with `pthread_gethandle` macro-poisoned. The encoder's writer waits no longer need a handle at all: `AsyncWriteLoop` now runs through a `std::packaged_task`, and both the bounded `Stop()` wait and the zero-timeout `Start()` poll go through `WriterFinishedWithin()` on the resulting `std::future<void>`, which reports "still running" for an invalid future so muxer ownership is never assumed free. `amf-headers` joins the Linux package list with its license as a required sentinel. Separator folding moves before the relative-path step, and linker resolution now asks the driver via `-print-prog-name` in addition to scanning siblings under both name spellings.
- **Diagnostics / coverage:** the `writer_finalize_timeout` line drops the now-meaningless Win32 `result=` and reports `timeout=`; the existing analyzer regexes key off the prefix and `phase=`, so both old and new lines still classify. `tests/test_thread_wait.cpp` asserts a finished thread's handle signals, a running thread's handle reports `WAIT_TIMEOUT` rather than `WAIT_FAILED` (exactly how the broken conversion misbehaved), and repeated queries are stable. A source-contract test fails if any first-party source outside `common/thread_wait.h` touches `native_handle` again, which is the only way a Windows-only change would notice. New coverage also pins host-independent clang-tidy scope keys and driver-resolved linker fingerprints.
- **Validation:** the complete clean Linux build now succeeds for the first time on this host - product, 29/29 test apps, PE mitigation/architecture/section/import verification, and both 7z packages. All 164 Python tool policy tests pass with one documented skip. The three `test_thread_wait.cpp` assertions were executed standalone under Wine with the real posix-threads toolchain (`ALL ASSERTIONS PASSED`) because the gtest binary itself cannot start on Linux; see the open question below.
- **Open / stale-risk:** lint on a non-canonical host silently rewrites the committed clang-tidy baseline - this run's clang-tidy 22.1.8 over a GCC database folded in `bugprone-exception-escape 30->0` and `bugprone-throwing-static-initialization 61->41`, which are analyzer differences rather than fixes and would then fail lint on Windows. The file was reverted; the guard itself is a lint-gate policy change and is left for a deliberate decision. `unit_tests.exe` still cannot run on a Linux host (`status c0000135`) - the staged MSYS2 FFmpeg needs roughly 46 dependency DLLs that `LINUX_MSYS2_PACKAGES` does not provide, so `--verify` cannot complete there. `tools/tests/test_ffmpeg_patch_utils.py` now skips loudly instead of erroring when the pinned FFmpeg checkout is absent, which is always the case on Linux. Neither affects hardening CI, which only cross-compiles.

### 2026-07-30 - Give every test app task its own object path so parallel Linux builds stop racing

- **Evidence / root cause:** the Linux hardening CI job failed in `compile_testapps` with
  `build/obj/testapps/x64/dx12_av_sync_test.o: file not recognized: file format not recognized` followed by
  `collect2: error: ld returned 1 exit status`, reported only as `1 test app(s) failed to build; first error: 1`.
  The object was not corrupt on disk for a compiler reason: two tasks were writing it at once. `compile_app` derived
  the object directory from `is_x86_compile_command(cmd)`, which recognizes only `--target=i686-w64`,
  `--sysroot=.../mingw32`, and MSYS2 `mingw32` paths. Linux hosts select the architecture through prefixed system
  compilers (`/usr/bin/i686-w64-mingw32-g++`) and add `x86_arch_flags = []`, so every Linux x86 command was
  classified x64 and mapped onto the x64 object path of its x64 twin. `add_task` appends the x64 and x86 variants of
  one app adjacently, so with CI's two workers each pair ran concurrently: 12 of 29 tasks shared an object with a
  live sibling. A reproduction on Arch confirmed the mechanism — pre-fix, 29 tasks produced only 17 objects in a
  single `x64/` directory; post-fix, 29 tasks produce 17 x64 plus 12 x86 objects. The same misclassification also
  handed x86 test apps the x64 build environment (wrong `PATH`/`CC`/`CXX`, and `DISABLE_CCACHE` unset).
- **Fix / invariants:** the architecture is now carried explicitly instead of re-derived from flag text.
  `make_cmd`/`make_cmd_x86` return a `TestAppCommand` that records the arch its call site chose, `add_task` stores it
  on the task, and both the task environment and `testapp_object_path()` read it. `is_x86_compile_command`
  additionally recognizes an `i686-w64-mingw32-` driver and `-m32`, so its remaining users — notably the
  `compile_commands.json` duplicate-resolution order that is documented to prefer the non-x86 variant — are correct
  on Linux too. `ensure_unique_testapp_objects()` fails the task list closed before any worker starts if two tasks
  ever target one object again, mirroring the guard `parallel_compile_varied` already had.
- **Diagnostics / coverage:** a per-task failure is now logged with its description and exception type, and the
  aggregate error names every failed app instead of only `first error: 1`. New `tools/tests/test_build_testapp_tasks.py`
  (registered as the seventeenth Python tool self-test group and in the Linux CI policy step) drives the real
  `compile_testapps` task construction on a simulated Linux host: distinct object paths per task, per-arch object
  directories, x86 tasks receiving the x86 environment, the collision guard rejecting duplicates, and the guard
  running before any worker. Six of its eight tests fail against the pre-fix code. CI's cache key also hashed only
  the `build.py` stub, never the `build_part_*.py` fragments that hold the actual package list, so it now hashes both.
- **Validation:** all 104 Python tool policy tests pass; the real `compile_testapps` stage — the exact stage CI failed
  in — completes on Arch with 29 built, 0 cached, and `file` confirms `x64/dx12_av_sync_test.o` as x86-64 COFF,
  `x86/dx12_av_sync_test.o` as i386 COFF, and both executables as PE32+/PE32 respectively.
- **Open / stale-risk:** the full clean Linux build cannot complete on Arch for an unrelated reason. Arch's
  `mingw-w64-gcc` 16.1.0 uses the **posix** thread model, so `std::thread::native_handle_type` is `pthread_t`
  (`unsigned long long`), while `mediaengine/video_encoder_part_005.inl:447` and `video_encoder_part_009.inl:241`
  assign `writerThread.native_handle()` straight to `HANDLE` for `WaitForSingleObject`. Ubuntu CI does not hit this
  because `update-alternatives` leaves `x86_64-w64-mingw32-g++` pointing at the **win32** model, where
  `native_handle_type` is `HANDLE`; MSYS2 clang64 likewise uses the Win32 thread API. A cast would be wrong — a
  winpthreads `pthread_t` is not a Win32 thread handle — so a real fix needs either
  `pthread_getw32threadhandle_np()` under an `if constexpr` model check or a thread that publishes its own duplicated
  Win32 handle. Untouched here: it is product code on the encoder finalize path, needs the `--verify` gate plus
  Windows runtime validation, and does not affect CI. Source anchors: `build_part_008.py`, `build_part_011.py`,
  `tools/tests/test_build_testapp_tasks.py`, `.github/workflows/hardening-ci.yml`.
