// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// Where the CaptureEngine package is, and which executable runs its helpers (libcengine 11, part 6).
//
// The package is the folder of the CaptureEngine binary this code is linked into: captureengine.exe or cengine.dll,
// never the running executable's, because with CE_FEATURE_SELF_ROLE_HOST a helper process is the client's own
// executable living somewhere else. Header-only and inline on purpose: every module gets its own copy, so the
// address lookup below always names the module the caller was linked into.
//
// The role host is the executable every helper launch uses (role processes, the per-app audio worker, the sensors
// bridge, elevated setup actions). The runtime sets it once at create; a process that never set it (a helper process
// itself, or captureengine.exe) relaunches its own executable, which is the role host by construction.

#include "common/platform/ansi_path.h"

#include <windows.h>

#include <mutex>
#include <string>

namespace ce::package {

inline HMODULE PackageModule() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&PackageModule), &module))
        module = nullptr;
    return module;
}

// Full path of captureengine.exe or cengine.dll, whichever this code is part of.
inline std::wstring PackageModulePath() {
    return ce::ansi_path::ModulePathW(PackageModule());
}

// The package folder, without a trailing separator.
inline std::wstring PackageDirectory() {
    return ce::ansi_path::ParentDirectoryW(PackageModulePath());
}

namespace detail {
inline std::mutex& RoleHostMutex() {
    static std::mutex mutex;
    return mutex;
}
inline std::wstring& RoleHostPathStorage() {
    static std::wstring path;
    return path;
}
}  // namespace detail

// Set by the runtime at create: <package>\captureengine.exe, or the client's executable with CE_FEATURE_SELF_ROLE_HOST.
inline void SetRoleHostExecutable(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(detail::RoleHostMutex());
    detail::RoleHostPathStorage() = path;
}

// The executable a helper launch runs: the runtime's choice, else this process's own executable.
inline std::wstring RoleHostExecutable() {
    {
        std::lock_guard<std::mutex> lock(detail::RoleHostMutex());
        if (!detail::RoleHostPathStorage().empty())
            return detail::RoleHostPathStorage();
    }
    return ce::ansi_path::ModulePathW(nullptr);
}

}  // namespace ce::package
