// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "common/platform/startup_imports.h"

#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ce::startup_imports::Image image;
    ce::startup_imports::Inspect([&](DWORD rva, std::span<std::byte> output) {
        if (rva > size || output.size() > size - rva) return false;
        std::memcpy(output.data(), data + rva, output.size());
        return true;
    }, image);
    return 0;
}
