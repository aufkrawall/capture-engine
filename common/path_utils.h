#pragma once

#include <filesystem>
#include <string>

namespace ce::path {

enum class MappedDriveResolutionSource {
    None,
    LiveMapping,
    RegistryMapping,
};

struct MappedDriveResolution {
    std::filesystem::path path;
    bool changed = false;
    bool driveAbsolute = false;
    wchar_t driveLetter = L'\0';
    MappedDriveResolutionSource source = MappedDriveResolutionSource::None;
    unsigned long liveMappingStatus = 0;
    unsigned long registryStatus = 0;
};

const char* MappedDriveResolutionSourceName(MappedDriveResolutionSource source);
bool IsDriveAbsolutePath(const std::filesystem::path& path);
std::filesystem::path ReplaceDriveRootWithRemotePath(const std::filesystem::path& path, const std::wstring& remoteRoot);
MappedDriveResolution ResolveMappedDrivePath(const std::filesystem::path& path);

// A path for the ANSI (A-suffixed) Windows APIs that configuration, logging and
// crash handling still use. When the path is representable in the active code
// page it is returned as that; otherwise (an installation below a folder whose
// name the code page cannot express, e.g. Cyrillic on a Western system) the
// existing file's 8.3 short name is used, which is ASCII. *exact reports
// whether the returned text names the path exactly; it is false only when
// neither form works, in which case the '?'-substituted text is returned.
std::string AnsiCompatiblePath(const std::wstring& widePath, bool* exact = nullptr);

}  // namespace ce::path
