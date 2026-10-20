#include "secure_dll_loading.h"

#include <algorithm>
#include <cwchar>
#include <mutex>
#include <string>
#include <system_error>
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
    HMODULE module =
        LoadLibraryExW(normalized.c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetError(module ? ERROR_SUCCESS : GetLastError(), error);
    return module;
}

ScopedDllDirectory::ScopedDllDirectory(const std::filesystem::path& directory, DWORD* error) {
    if (!IsAbsoluteDirectory(directory)) {
        SetError(ERROR_BAD_PATHNAME, error);
        return;
    }
    cookie_ = AddDllDirectory(directory.lexically_normal().c_str());
    SetError(cookie_ ? ERROR_SUCCESS : GetLastError(), error);
}

ScopedDllDirectory::~ScopedDllDirectory() {
    if (cookie_)
        RemoveDllDirectory(cookie_);
}

HMODULE LoadLibraryFromScopedDirectory(const std::filesystem::path& path, const std::filesystem::path& directory,
                                       DWORD* error) {
    if (!path.is_absolute()) {
        SetError(ERROR_BAD_PATHNAME, error);
        return nullptr;
    }
    ScopedDllDirectory searchScope(directory, error);
    if (!searchScope)
        return nullptr;
    const std::wstring normalized = path.lexically_normal().wstring();
    HMODULE module =
        LoadLibraryExW(normalized.c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetError(module ? ERROR_SUCCESS : GetLastError(), error);
    return module;
}

bool PreloadScopedRuntimeDlls(const std::filesystem::path& directory, const std::vector<std::wstring>& prefixes,
                              DWORD* error) {
    ScopedDllDirectory searchScope(directory, error);
    if (!searchScope)
        return false;

    std::error_code ec;
    std::filesystem::directory_iterator entries(directory, ec);
    if (ec) {
        SetError(ERROR_PATH_NOT_FOUND, error);
        return false;
    }
    for (; entries != std::filesystem::directory_iterator(); entries.increment(ec)) {
        if (ec) {
            SetError(ERROR_PATH_NOT_FOUND, error);
            return false;
        }
        const std::filesystem::path& file = entries->path();
        if (file.extension() != L".dll")
            continue;
        const std::wstring name = file.filename().wstring();
        const bool wanted = std::any_of(prefixes.begin(), prefixes.end(), [&name](const std::wstring& prefix) {
            return name.size() >= prefix.size() && _wcsnicmp(name.c_str(), prefix.c_str(), prefix.size()) == 0;
        });
        if (!wanted || GetModuleHandleW(name.c_str()))
            continue;
        HMODULE module = LoadLibraryExW(file.lexically_normal().wstring().c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS |
                                            LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            SetError(GetLastError(), error);
            return false;
        }
    }
    SetError(ERROR_SUCCESS, error);
    return true;
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
