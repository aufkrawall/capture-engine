# Fuzzing

## Available targets

| Target | Source | Purpose |
|---|---|---|
| Config parser | `tests/fuzz_config_parser_libfuzzer.cpp` | Fuzz `LoadConfig()` with arbitrary byte sequences |
| IPC deserialization | `tests/fuzz_ipc_deserialize.cpp` | Fuzz `ValidatePayload`/`ValidateOpcodePayload` with arbitrary messages |

## Building and running

All targets use the MSYS2 Clang toolchain with libFuzzer (`-fsanitize=fuzzer`).

### Config parser

```powershell
cd <project_root>
& "build\msys64\clang64\bin\clang++.exe" -fsanitize=fuzzer,address -o fuzz_config.exe `
    tests/fuzz_config_parser_libfuzzer.cpp common/config.cpp -I common -I . -lshlwapi

# Run with default corpus
mkdir -p tests/fuzz_corpus/config
.\fuzz_config.exe -max_total_time=300 tests/fuzz_corpus/config/
```

### IPC deserialization

```powershell
& "build\msys64\clang64\bin\clang++.exe" -fsanitize=fuzzer,address -o fuzz_ipc.exe `
    tests/fuzz_ipc_deserialize.cpp common/process_ipc.cpp -I common -I .

mkdir -p tests/fuzz_corpus/ipc
.\fuzz_ipc.exe -max_total_time=300 tests/fuzz_corpus/ipc/
```

## Crash triage

When libFuzzer finds a crash it writes the reproducer input to a file in the corpus directory or the current directory as `crash-<sha1>`. To minimize:

```powershell
.\fuzz_config.exe -minimize_crash=1 -max_total_time=60 crash-<sha1>
```

Add the minimized crash input to the corpus and create a regression test:

```cpp
// In tests/test_config_fuzz.cpp or a new test file
TEST(ConfigFuzzTest, Regression_CrashDescription) {
    const std::string path = "...";
    WriteFuzzFile(path, {0x...});  // bytes from minimized crash
    AppConfig config;
    EXPECT_NO_THROW(LoadConfig(path, config));
}
```

## Corpus

The initial corpus for config parsing lives in `tests/fuzz_corpus/config/`. It contains well-formed `.ini` files, edge cases (empty, comments only, max-size values), and previously discovered crash inputs.

The IPC corpus (`tests/fuzz_corpus/ipc/`) contains valid `ProcessMessage` structures with various opcodes and payload sizes.

## Coverage

To generate coverage reports:

```powershell
# Build with coverage instrumentation
& "build\msys64\clang64\bin\clang++.exe" -fprofile-instr-generate -fcoverage-mapping `
    -o fuzz_config_cov.exe tests/fuzz_config_parser_libfuzzer.cpp common/config.cpp -I common -I .

# Run with a seed corpus
.\fuzz_config_cov.exe -runs=10000 tests/fuzz_corpus/config/

# Generate coverage report
& "build\msys64\clang64\bin\llvm-profdata.exe" merge -spacing default.profraw -o default.profdata
& "build\msys64\clang64\bin\llvm-cov.exe" show ./fuzz_config_cov.exe -instr-profile=default.profdata
```
