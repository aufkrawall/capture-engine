// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "hook/hooking/iat_import_table.h"
#include "hook/runtime/pe_file_exports.h"

#include <cstring>
#include <string>
#include <vector>

// The Streamline takeover reads the configured interposer's export names from
// disk and walks the game's sl.interposer.dll imports; both treat every header
// field as untrusted. The input is used as file bytes and, at the same time,
// as a mapped image (raw offset == RVA).
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::vector<std::string> names;
    ce::pe_file_exports::ParseExportNames(data, size, &names);

    if (size < sizeof(IMAGE_DOS_HEADER) || size > 4 * 1024 * 1024) {
        return 0;
    }
    std::vector<uint8_t> image(data, data + size);
    IMAGE_DOS_HEADER dos = {};
    std::memcpy(&dos, image.data(), sizeof(dos));
    if (dos.e_lfanew <= 0 || static_cast<size_t>(dos.e_lfanew) > size ||
        sizeof(IMAGE_NT_HEADERS64) > size - static_cast<size_t>(dos.e_lfanew)) {
        return 0;
    }
    IMAGE_NT_HEADERS64 nt = {};
    std::memcpy(&nt, image.data() + dos.e_lfanew, sizeof(nt));
    const auto& directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    size_t visits = 0;
    IATHook::detail::ForEachImportOf(
        image.data(), image.size(), directory.VirtualAddress, directory.Size, "sl.interposer.dll",
        [&](IATHook::detail::ImportNameKind, const char* name, IMAGE_THUNK_DATA*) {
            visits += name ? std::strlen(name) : 1;
        },
        nullptr);
    return visits == SIZE_MAX ? 1 : 0;
}
