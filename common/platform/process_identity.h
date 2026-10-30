#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// Translate an NT kernel device path (such as \Device\HarddiskVolume3\Dir\App.exe
// or \Device\Mup\server\share\App.exe) to a Win32 path when a matching drive
// mapping is present, preserving component boundaries so Volume1 never matches
// Volume10.
std::wstring TranslateNtDevicePathToWin32(
    std::wstring_view ntPath, const std::vector<std::pair<std::wstring, std::wstring>>& driveMappings);

// Resolve the executable path and basename from the kernel's per-PID image
// record without opening a handle to the target process.
ProcessIdentityResult QueryProcessIdentityBySystemProcessId(uint32_t processId);

// Resolve the executable path and basename using Windows' documented
// limited-query process interface, falling back to the kernel per-PID image
// query when handle access is denied.
ProcessIdentityResult QueryProcessIdentity(uint32_t processId);

}  // namespace ce::process
