# llm-wiki Log

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
