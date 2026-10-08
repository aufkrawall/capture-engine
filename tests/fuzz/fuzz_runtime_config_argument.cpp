// Fuzz the pure untrusted --config argument boundary. No filesystem access or
// process launch: length-delimited UTF-16 chunks form parsed Windows arguments.
#include "common/platform/runtime_package_paths.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::vector<std::wstring> arguments{L"fixture.exe"};
    size_t position = 0;
    while (position + 1 < size && arguments.size() < 32) {
        const size_t requested = data[position++];
        const size_t characters = std::min(requested, (size - position) / 2);
        std::wstring value;
        value.reserve(characters);
        for (size_t index = 0; index < characters; ++index) {
            const uint16_t character = static_cast<uint16_t>(static_cast<uint16_t>(data[position]) |
                                                             (static_cast<uint16_t>(data[position + 1]) << 8));
            position += 2;
            value.push_back(static_cast<wchar_t>(character));
        }
        arguments.push_back(std::move(value));
    }
    (void)ce::runtime::ReadConfigurationArgument(arguments);
    return 0;
}
