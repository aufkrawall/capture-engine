#pragma once

#include <cstdint>
#include <string>

namespace ce::process {

struct ProcessIdentityResult {
    // UTF-8 so selectors read from UTF-8 config.ini stay lossless on every
    // Windows code page.
    std::string imageName;
    std::string imagePath;
    unsigned long error = 0;

    explicit operator bool() const {
        return !imageName.empty();
    }
};

// Resolve the executable path and basename using Windows' documented
// limited-query process interface. This never reads or modifies target-process
// memory.
ProcessIdentityResult QueryProcessIdentity(uint32_t processId);

}  // namespace ce::process
