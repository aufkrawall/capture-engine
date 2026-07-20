// libFuzzer harness for config parser
// Build: clang++ -fsanitize=fuzzer,address -o fuzz_config
//   tests/fuzz_config_parser_libfuzzer.cpp common/config.cpp -I common -I ..
// Run: ./fuzz_config -max_total_time=300 tests/fuzz_corpus/config/

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "../common/config.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    char tempPathBuf[MAX_PATH];
    GetTempPathA(MAX_PATH, tempPathBuf);
    std::string path = std::string(tempPathBuf) + "ce_fuzz_config.ini";

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return 0;
    fwrite(data, 1, size, f);
    fclose(f);

    AppConfig config;
    LoadConfig(path, config);

    std::filesystem::remove(path);
    return 0;
}
