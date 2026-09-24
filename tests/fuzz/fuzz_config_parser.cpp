// libFuzzer harness for the configuration parser.
//
// Target: LoadConfig(), which parses an untrusted .ini file off disk
// (common/config.cpp). LoadConfig takes a path rather than a buffer, so each
// iteration materialises the fuzz input into a private temp file. The UTF-8
// INI reader (common/config_ini_reader.cpp) is also driven directly on the raw
// bytes: LoadConfig only reaches it for input that is valid UTF-8 or carries a
// BOM, and the grammar must hold for every byte sequence.
//
// Built and run by build.py --run-fuzz; see llm-wiki/fuzzing.md.

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "../../common/config.h"
#include "../../common/config_ini_reader.h"

namespace {

// One fixed filename would collide between concurrently running fuzzer workers
// and between parallel build stages, producing false crashes and lost coverage.
// Bind the scratch file to this process instead.
const std::string& ScratchConfigPath() {
    static const std::string path = [] {
        char tempDir[MAX_PATH]{};
        const DWORD length = GetTempPathA(MAX_PATH, tempDir);
        std::string base = (length > 0 && length < MAX_PATH) ? std::string(tempDir, length) : std::string(".\\");
        base += "ce_fuzz_config_";
        base += std::to_string(GetCurrentProcessId());
        base += ".ini";
        return base;
    }();
    return path;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string& path = ScratchConfigPath();

    const HANDLE file =
        CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    if (size > 0) {
        DWORD written = 0;
        WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr);
    }
    CloseHandle(file);

    AppConfig config;
    LoadConfig(path, config);

    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    const ce::config_text::IniDocument document = ce::config_text::ParseUtf8Ini(bytes);
    for (const std::string& name : ce::config_text::IniSectionNames(document, 932)) {
        std::vector<std::string> lines;
        ce::config_text::IniSectionLines(document, name, 932, &lines);
        std::string value;
        ce::config_text::LookupIniValue(document, name, "output_dir", 932, &value);
    }

    return 0;
}

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    // Remove the scratch file when the process exits normally; a crash deliberately
    // leaves it behind next to the libFuzzer reproducer for triage.
    atexit([] { DeleteFileA(ScratchConfigPath().c_str()); });
    return 0;
}
