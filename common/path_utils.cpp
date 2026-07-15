#include "path_utils.h"
#include "secure_dll_loading.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <vector>

namespace ce::path {
namespace {

bool IsSlash(wchar_t ch) {
    return ch == L'\\' || ch == L'/';
}

bool IsDriveLetter(wchar_t ch) {
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
}

std::wstring TrimTrailingSlashes(std::wstring value) {
    while (value.size() > 2 && IsSlash(value.back())) {
        value.pop_back();
    }
    return value;
}

std::wstring ExpandRegistryStringIfNeeded(const std::wstring& value, DWORD type) {
    if (type != REG_EXPAND_SZ) {
        return value;
    }

    const DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (required == 0) {
        return value;
    }

    std::wstring expanded(required, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(value.c_str(), expanded.data(), required);
    if (written == 0 || written > required) {
        return value;
    }
    if (!expanded.empty() && expanded.back() == L'\0') {
        expanded.pop_back();
    }
    return expanded;
}

bool QueryLiveMappedDriveRemote(wchar_t driveLetter, std::wstring& remoteRoot, DWORD& status) {
    using WNetGetConnectionWFn = DWORD(WINAPI*)(LPCWSTR, LPWSTR, LPDWORD);

    HMODULE mpr = GetModuleHandleW(L"mpr.dll");
    if (!mpr) {
        mpr = ce::security::LoadSystemLibrary(L"mpr.dll");
    }
    if (!mpr) {
        status = GetLastError();
        return false;
    }

    auto wnetGetConnection = reinterpret_cast<WNetGetConnectionWFn>(GetProcAddress(mpr, "WNetGetConnectionW"));
    if (!wnetGetConnection) {
        status = ERROR_PROC_NOT_FOUND;
        return false;
    }

    wchar_t localName[] = {static_cast<wchar_t>(std::towupper(driveLetter)), L':', L'\0'};
    DWORD bufferChars = 512;
    std::vector<wchar_t> buffer(bufferChars, L'\0');
    status = wnetGetConnection(localName, buffer.data(), &bufferChars);
    if (status == ERROR_MORE_DATA && bufferChars > 0) {
        buffer.assign(bufferChars + 1, L'\0');
        status = wnetGetConnection(localName, buffer.data(), &bufferChars);
    }
    if (status != NO_ERROR) {
        return false;
    }

    remoteRoot.assign(buffer.data());
    remoteRoot = TrimTrailingSlashes(remoteRoot);
    return !remoteRoot.empty();
}

bool QueryRegistryMappedDriveRemote(wchar_t driveLetter, std::wstring& remoteRoot, DWORD& status) {
    wchar_t subkey[] = L"Network\\Z";
    subkey[8] = static_cast<wchar_t>(std::towupper(driveLetter));

    DWORD type = 0;
    DWORD bytes = 0;
    status = RegGetValueW(HKEY_CURRENT_USER, subkey, L"RemotePath", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type,
                          nullptr, &bytes);
    if (status != ERROR_SUCCESS || bytes == 0) {
        return false;
    }

    std::vector<wchar_t> buffer((bytes / sizeof(wchar_t)) + 1, L'\0');
    status = RegGetValueW(HKEY_CURRENT_USER, subkey, L"RemotePath", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type,
                          buffer.data(), &bytes);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    remoteRoot.assign(buffer.data());
    remoteRoot = TrimTrailingSlashes(ExpandRegistryStringIfNeeded(remoteRoot, type));
    return !remoteRoot.empty();
}

}  // namespace

const char* MappedDriveResolutionSourceName(MappedDriveResolutionSource source) {
    switch (source) {
        case MappedDriveResolutionSource::LiveMapping:
            return "live_mapping";
        case MappedDriveResolutionSource::RegistryMapping:
            return "registry_mapping";
        case MappedDriveResolutionSource::None:
        default:
            return "none";
    }
}

bool IsDriveAbsolutePath(const std::filesystem::path& path) {
    const std::wstring value = path.wstring();
    return value.size() >= 3 && IsDriveLetter(value[0]) && value[1] == L':' && IsSlash(value[2]);
}

std::filesystem::path ReplaceDriveRootWithRemotePath(const std::filesystem::path& path,
                                                     const std::wstring& remoteRoot) {
    const std::wstring value = path.wstring();
    if (!IsDriveAbsolutePath(path) || remoteRoot.empty()) {
        return path;
    }

    std::wstring combined = TrimTrailingSlashes(remoteRoot);
    size_t remainderStart = 2;
    while (remainderStart < value.size() && IsSlash(value[remainderStart])) {
        ++remainderStart;
    }
    if (remainderStart < value.size()) {
        combined.push_back(L'\\');
        combined.append(value.substr(remainderStart));
    }
    return std::filesystem::path(combined);
}

MappedDriveResolution ResolveMappedDrivePath(const std::filesystem::path& path) {
    MappedDriveResolution result;
    result.path = path;
    result.driveAbsolute = IsDriveAbsolutePath(path);
    if (!result.driveAbsolute) {
        return result;
    }

    const std::wstring value = path.wstring();
    result.driveLetter = static_cast<wchar_t>(std::towupper(value[0]));

    std::wstring remoteRoot;
    if (QueryLiveMappedDriveRemote(result.driveLetter, remoteRoot, result.liveMappingStatus)) {
        result.path = ReplaceDriveRootWithRemotePath(path, remoteRoot);
        result.changed = result.path != path;
        result.source = result.changed ? MappedDriveResolutionSource::LiveMapping : MappedDriveResolutionSource::None;
        return result;
    }

    if (QueryRegistryMappedDriveRemote(result.driveLetter, remoteRoot, result.registryStatus)) {
        result.path = ReplaceDriveRootWithRemotePath(path, remoteRoot);
        result.changed = result.path != path;
        result.source =
            result.changed ? MappedDriveResolutionSource::RegistryMapping : MappedDriveResolutionSource::None;
    }
    return result;
}

}  // namespace ce::path
