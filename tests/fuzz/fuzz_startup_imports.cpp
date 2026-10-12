// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "common/platform/startup_imports.h"
#include "common/platform/loaded_pe_image.h"

#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ce::startup_imports::Image image;
    ce::startup_imports::Inspect([&](DWORD rva, std::span<std::byte> output) {
        if (rva > size || output.size() > size - rva) return false;
        std::memcpy(output.data(), data + rva, output.size());
        return true;
    }, image);
    if (size && size <= 1024 * 1024) {
        void* mapped = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (mapped) {
            std::memcpy(mapped, data, size);
            const auto loaded = ce::pe::LoadedImage::Open(mapped);
            if (loaded) {
                const auto& directory = loaded->headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
                loaded->Read<IMAGE_EXPORT_DIRECTORY>(directory.VirtualAddress);
            }
            VirtualFree(mapped, 0, MEM_RELEASE);
        }
    }
    return 0;
}
