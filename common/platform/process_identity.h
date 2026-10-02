#pragma once

#include <cstdint>
#include <string>

namespace ce::process {

struct ProcessIdentityResult {
    std::string imageName;
    unsigned long error = 0;

    explicit operator bool() const {
        return !imageName.empty();
    }
};

// Resolve only the executable basename using Windows' documented limited-query
// process interface. This never reads or modifies target-process memory.
ProcessIdentityResult QueryProcessIdentity(uint32_t processId);

}  // namespace ce::process
