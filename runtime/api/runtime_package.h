// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include "include/cengine/cengine.h"

#include <windows.h>

#include <optional>
#include <string>

namespace ce::capi {

// Where a runtime's files are: the package directory the client named, or the directory of the module that
// contains the runtime code, and the settings file (the client's, or <package>\config.ini).
struct PackageLayout {
    std::wstring roleHost;                // <package>\captureengine.exe: the executable the helper roles run from
    std::string configPath;               // active-code-page <package>\config.ini
    std::wstring configPathWide;          // the same file in UTF-16 (the settings store opens it)
    std::string logsRoot;                 // active-code-page <package>\logs: the diagnostics session's root
    HMODULE windowModule = nullptr;       // the module containing the runtime code: owns the overlay's window classes
};

// `packageDir` is the descriptor's package_dir (UTF-8), nullopt for the default. CE_E_INVALID_ARGUMENT for a
// relative package_dir. CE_E_PACKAGE when the module path cannot be read, the role host is missing, or the
// package path cannot be expressed in the active code page (its ANSI configuration path would name another file).
// `reason` (optional) receives a static diagnostic for logging.
// `settingsPath` is the descriptor's settings_path (UTF-8, absolute; CE_E_INVALID_ARGUMENT otherwise), nullopt for
// <package>\config.ini; it is resolved the way captureengine.exe resolves --config=.
ce_status_t ResolvePackage(const std::optional<std::string>& packageDir, const std::optional<std::string>& settingsPath,
                           PackageLayout& out, const char** reason = nullptr);

}  // namespace ce::capi
