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

}  // namespace ce::path
