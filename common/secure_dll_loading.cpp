#include "secure_dll_loading.h"

#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

namespace ce::security {
namespace {

std::once_flag g_defaultDirectoriesOnce;
bool g_defaultDirectoriesReady = false;
DWORD g_defaultDirectoriesError = ERROR_SUCCESS;
std::mutex g_directoryMutex;
std::vector<std::wstring> g_registeredDirectories;
std::vector<DLL_DIRECTORY_COOKIE> g_directoryCookies;

void SetError(DWORD value, DWORD* error) {
    if (error)
        *error = value;
    SetLastError(value);
}

bool IsAbsoluteDirectory(const std::filesystem::path& directory) {
    std::error_code ec;
    return directory.is_absolute() && std::filesystem::is_directory(directory, ec) && !ec;
}

bool PathsEqual(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

}  // namespace

bool EnsureSecureDllSearchDirectory(const std::filesystem::path& directory, DWORD* error) {
    if (!IsAbsoluteDirectory(directory)) {
        SetError(ERROR_BAD_PATHNAME, error);
        return false;
    }

    std::call_once(g_defaultDirectoriesOnce, [] {
        g_defaultDirectoriesReady =
            SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS |
                                     LOAD_LIBRARY_SEARCH_SYSTEM32) != FALSE;
        if (!g_defaultDirectoriesReady)
            g_defaultDirectoriesError = GetLastError();
    });
    if (!g_defaultDirectoriesReady) {
        SetError(g_defaultDirectoriesError, error);
        return false;
    }

    const std::wstring normalized = directory.lexically_normal().wstring();
    std::lock_guard<std::mutex> lock(g_directoryMutex);
    for (const std::wstring& existing : g_registeredDirectories) {
        if (PathsEqual(existing, normalized)) {
            SetError(ERROR_SUCCESS, error);
            return true;
        }
    }

    const DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(normalized.c_str());
    if (!cookie) {
        SetError(GetLastError(), error);
        return false;
    }
    g_registeredDirectories.push_back(normalized);
    g_directoryCookies.push_back(cookie);
    SetError(ERROR_SUCCESS, error);
    return true;
}

HMODULE LoadLibraryFromSecurePath(const std::filesystem::path& path, DWORD* error) {
    if (!path.is_absolute()) {
        SetError(ERROR_BAD_PATHNAME, error);
        return nullptr;
    }
    const std::wstring normalized = path.lexically_normal().wstring();
    HMODULE module = LoadLibraryExW(normalized.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS |
                                        LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetError(module ? ERROR_SUCCESS : GetLastError(), error);
    return module;
}

HMODULE LoadSystemLibrary(const wchar_t* moduleName, DWORD* error) {
    if (!moduleName || !*moduleName || wcschr(moduleName, L'\\') || wcschr(moduleName, L'/')) {
        SetError(ERROR_INVALID_NAME, error);
        return nullptr;
    }
    HMODULE module = LoadLibraryExW(moduleName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetError(module ? ERROR_SUCCESS : GetLastError(), error);
    return module;
}

}  // namespace ce::security
