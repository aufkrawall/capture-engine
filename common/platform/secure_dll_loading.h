#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace ce::security {

// Permanently narrows this process to application, explicitly trusted, and
// System32 DLL directories, then adds one absolute private runtime directory.
// The directory remains registered because delay-loaded imports may resolve at
// any later point in the process lifetime.
bool EnsureSecureDllSearchDirectory(const std::filesystem::path& directory, DWORD* error = nullptr);

// Loads one explicitly named DLL and its dependencies without consulting the
// current directory or PATH. The private runtime directory must have been
// registered with EnsureSecureDllSearchDirectory first when dependencies live
// outside the DLL's own directory.
HMODULE LoadLibraryFromSecurePath(const std::filesystem::path& path, DWORD* error = nullptr);

HMODULE LoadSystemLibrary(const wchar_t* moduleName, DWORD* error = nullptr);

// Makes `directory` searchable for LoadLibraryEx calls with LOAD_LIBRARY_SEARCH_USER_DIRS, for the lifetime of this
// object only. Unlike EnsureSecureDllSearchDirectory it leaves the process default search order alone: a role that
// runs on a client's thread (DR-39) must not narrow or widen the client's DLL search.
class ScopedDllDirectory {
public:
    explicit ScopedDllDirectory(const std::filesystem::path& directory, DWORD* error = nullptr);
    ~ScopedDllDirectory();
    ScopedDllDirectory(const ScopedDllDirectory&) = delete;
    ScopedDllDirectory& operator=(const ScopedDllDirectory&) = delete;

    explicit operator bool() const {
        return cookie_ != nullptr;
    }

private:
    DLL_DIRECTORY_COOKIE cookie_ = nullptr;
};

// Loads one DLL by absolute path with `directory` searchable for this load only, so the DLL's dependencies in that
// directory resolve while every later LoadLibrary in the process searches as it did before.
HMODULE LoadLibraryFromScopedDirectory(const std::filesystem::path& path, const std::filesystem::path& directory,
                                       DWORD* error = nullptr);

// Loads each DLL of `directory` whose file name starts with one of `prefixes` (versioned runtime names such as
// "avcodec-"), by full path, with `directory` searchable for these loads only. The modules stay loaded: a delay-loaded
// import of the same name then resolves to the loaded module, which needs no search. A module already loaded under
// its name is left alone, so repeated calls add no references.
bool PreloadScopedRuntimeDlls(const std::filesystem::path& directory, const std::vector<std::wstring>& prefixes,
                              DWORD* error = nullptr);

}  // namespace ce::security
