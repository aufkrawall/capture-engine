#pragma once

#include <filesystem>
#include <string_view>

namespace ce::startup {

inline std::filesystem::path ServiceDirectoryForExecutable(const std::filesystem::path& executable) {
    if (!executable.is_absolute() || executable.filename().empty())
        return {};
    return executable.parent_path() / L"ElevationService";
}

// The administrator-owned SCM registration is authoritative for a previous
// install's location, including the legacy Program Files/CaptureEngine root.
// Only our exact nonce runtime layout is eligible for replacement/removal.
inline std::filesystem::path RegisteredServiceRuntime(std::wstring_view binary) {
    if (binary.size() < 3 || binary.front() != L'"' || binary.back() != L'"')
        return {};
    const auto pathText = binary.substr(1, binary.size() - 2);
    if (pathText.find(L'"') != std::wstring_view::npos)
        return {};
    const std::filesystem::path executable(pathText);
    if (!executable.is_absolute() || executable.filename() != L"captureengine_elevation_service.exe")
        return {};
    for (const auto& component : executable)
        if (component == L"." || component == L"..")
            return {};
    const auto runtime = executable.parent_path();
    const auto directory = runtime.parent_path();
    const std::wstring leaf = runtime.filename().wstring();
    if (directory.filename() != L"ElevationService" || leaf.size() != 40 || leaf.substr(0, 8) != L"runtime-")
        return {};
    for (wchar_t character : leaf.substr(8))
        if (!((character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f')))
            return {};
    return runtime;
}

}  // namespace ce::startup
