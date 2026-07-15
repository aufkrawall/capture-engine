#pragma once

#include <windows.h>

#include <filesystem>

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

}  // namespace ce::security
