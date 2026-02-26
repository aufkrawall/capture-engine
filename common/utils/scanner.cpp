#include "scanner.h"
#include <psapi.h>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace Scanner {

std::vector<int> ParsePattern(const char* pattern) {
    std::vector<int> bytes;
    std::stringstream ss(pattern);
    std::string segment;
    while (std::getline(ss, segment, ' ')) {
        if (segment == "?" || segment == "??") {
            bytes.push_back(-1);
        } else {
            try {
                bytes.push_back(std::stoi(segment, nullptr, 16));
            } catch (...) {
                bytes.push_back(-1);
            }
        }
    }
    return bytes;
}

uintptr_t Scan(HMODULE module, const char* pattern) {
    if (!module)
        return 0;

    MODULEINFO moduleInfo;
    if (!GetModuleInformation(GetCurrentProcess(), module, &moduleInfo, sizeof(moduleInfo))) {
        return 0;
    }

    uint8_t* start = (uint8_t*)moduleInfo.lpBaseOfDll;
    size_t size = moduleInfo.SizeOfImage;
    std::vector<int> patternBytes = ParsePattern(pattern);

    for (size_t i = 0; i < size - patternBytes.size(); ++i) {
        bool found = true;
        for (size_t j = 0; j < patternBytes.size(); ++j) {
            if (patternBytes[j] != -1 && start[i + j] != (uint8_t)patternBytes[j]) {
                found = false;
                break;
            }
        }
        if (found) {
            return (uintptr_t)(start + i);
        }
    }
    return 0;
}

uintptr_t ScanBase(const char* moduleName, const char* pattern) {
    HMODULE hMod = GetModuleHandleA(moduleName);
    return Scan(hMod, pattern);
}

// Implement ScanForStringRef
uintptr_t ScanForStringRef(HMODULE module, const char* string) {
    if (!module || !string)
        return 0;

    MODULEINFO moduleInfo;
    if (!GetModuleInformation(GetCurrentProcess(), module, &moduleInfo, sizeof(moduleInfo))) {
        return 0;
    }

    uint8_t* base = (uint8_t*)moduleInfo.lpBaseOfDll;
    size_t size = moduleInfo.SizeOfImage;

    // 1. Find the string first
    std::vector<int> stringBytes;
    for (size_t i = 0; string[i]; i++)
        stringBytes.push_back(string[i]);
    stringBytes.push_back(0);  // Null terminator

    uintptr_t stringAddr = 0;

    for (size_t i = 0; i < size - stringBytes.size(); ++i) {
        bool found = true;
        for (size_t j = 0; j < stringBytes.size(); ++j) {
            if (base[i + j] != (uint8_t)stringBytes[j]) {
                found = false;
                break;
            }
        }
        if (found) {
            stringAddr = (uintptr_t)(base + i);
            break;  // Found the string
        }
    }

    if (!stringAddr)
        return 0;

    // 2. Find LEA/MOV referencing this address (x64 RIP-relative)
    // LEA RDX, [RIP + disp] -> 48 8D 15 ?? ?? ?? ?? (or similar reg)
    // MOV RDX, [RIP + disp] -> 48 8B 15 ?? ?? ?? ??
    // Common pattern for passing string arg 2: LEA RDX, [String]
    // Opcode can vary (48 8D 15, 48 8D 0D, 4C 8D 05, etc.)

    // We will bruteforce scan for any 4-byte displacement that resolves to
    // stringAddr This is slow but robust.

    uint8_t* p = base;
    uint8_t* end = base + size - 7;

    for (; p < end; p++) {
        // Check for LEA/MOV instruction patterns?
        // Or just check if (p + 7 + *(int32*)(p+3)) == stringAddr
        // Assuming 7 byte instruction length (opcode 3 + disp 4)
        // Common LEA: 48 8D 15 (RDX) or 48 8D 0D (RCX) or 4C 8D 05 (R8)

        // Optimization: Only check if high bytes look like an instruction?
        // Let's just check the displacement logic for typical instruction sizes (7
        // bytes)

        // Pattern: Op1 Op2 Op3 Disp Disp Disp Disp
        // Check offset at +3
        int32_t disp = *(int32_t*)(p + 3);
        if ((uintptr_t)(p + 7 + disp) == stringAddr) {
            // Check if it looks like LEA
            if (p[0] == 0x48 || p[0] == 0x4C) {  // REX prefix
                if (p[1] == 0x8D) {              // LEA
                    return (uintptr_t)p;
                }
            }
        }
    }

    return 0;  // Not found
}

std::vector<uintptr_t> ScanForPointer(HMODULE module, uintptr_t target) {
    std::vector<uintptr_t> results;
    if (!module || !target)
        return results;

    MODULEINFO moduleInfo;
    if (!GetModuleInformation(GetCurrentProcess(), module, &moduleInfo, sizeof(moduleInfo))) {
        return results;
    }

    uint8_t* start = (uint8_t*)moduleInfo.lpBaseOfDll;
    size_t size = moduleInfo.SizeOfImage;

    // Scan for 8-byte aligned pointers
    // We iterate by 8 bytes
    uintptr_t* pStart = (uintptr_t*)start;
    uintptr_t* pEnd = (uintptr_t*)(start + size - 8);

    for (uintptr_t* p = pStart; p <= pEnd; p++) {
        // Check alignment risk? standard scan is byte-by-byte but pointers usually
        // aligned. Let's do byte scan for safety but cast to uintptr_t Actually,
        // scanning byte-by-byte for a 64-bit value is safer to avoid alignment
        // faults (though x64 handles unaligned loads fine mostly, but valid ptrs
        // are aligned). Let's stick to 8-byte alignment for speed and probability.
        if (*p == target) {
            results.push_back((uintptr_t)p);
        }
    }

    return results;
}
}  // namespace Scanner
