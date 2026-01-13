#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include <optional>

namespace Scanner {
    // Basic AOB scan
    // Pattern format: "AA BB ?? DD" (hex bytes, ?? for wildcards)
    uintptr_t Scan(HMODULE module, const char* pattern);

    // Scan relative to module base
    uintptr_t ScanBase(const char* moduleName, const char* pattern);

    // Scan for string reference (find code that references this string)
    // useful for finding console commands
    uintptr_t ScanForStringRef(HMODULE module, const char* string);

    // Scan for direct pointer references to a target address
    std::vector<uintptr_t> ScanForPointer(HMODULE module, uintptr_t target);
}
