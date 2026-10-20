// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/api/runtime_package.h"

#include "common/logging/logging.h"
#include "common/platform/ansi_path.h"
#include "common/platform/runtime_package_paths.h"
#include "runtime/api/capi_text.h"

#include <filesystem>

namespace ce::capi {
namespace {

// Any address inside this module names the module to GetModuleHandleExW.
HMODULE ModuleContainingThisCode() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleContainingThisCode), &module);
    return module;
}

}  // namespace

ce_status_t ResolvePackage(const std::optional<std::string>& packageDir, PackageLayout& out, const char** reason) {
    const char* ignored = nullptr;
    const char*& why = reason ? *reason : ignored;
    why = nullptr;
    out.windowModule = ModuleContainingThisCode();

    std::wstring directory;
    if (packageDir) {
        if (!Utf8ToWideStrict(*packageDir, directory)) {
            why = "package_dir is not valid UTF-8";
            return CE_E_INVALID_ARGUMENT;
        }
        if (!std::filesystem::path(directory).is_absolute()) {
            why = "package_dir must be an absolute path";
            return CE_E_INVALID_ARGUMENT;
        }
    } else {
        directory = ce::ansi_path::ParentDirectoryW(ce::ansi_path::ModulePathW(out.windowModule));
        if (directory.empty()) {
            why = "the module path of the runtime code cannot be read";
            return CE_E_PACKAGE;
        }
    }

    const std::wstring roleHost = (std::filesystem::path(directory) / L"captureengine.exe").wstring();
    const DWORD attributes = GetFileAttributesW(roleHost.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        LogWarn("[CApi] role host is missing: %s", ce::ansi_path::CompatiblePath(roleHost).c_str());
        why = "captureengine.exe is missing from the package directory";
        return CE_E_PACKAGE;
    }

    // The ANSI configuration path the INI loader and the logs use, derived the way the executable derives it.
    ce::runtime::PackagePathError error = ce::runtime::PackagePathError::None;
    const auto paths =
        ce::runtime::RuntimePackagePaths::FromExecutable(roleHost, ce::runtime::ConfigurationArgument{}, error);
    if (!paths) {
        why = "the package layout cannot be resolved";
        return CE_E_PACKAGE;
    }
    if (!paths->EncodingExact()) {
        LogWarn("[CApi] package path is not representable in the active code page: %s",
                paths->Configuration().c_str());
        why = "the package path is not representable in the active code page";
        return CE_E_PACKAGE;
    }
    out.roleHost = roleHost;
    out.configPath = paths->Configuration();
    return CE_OK;
}

}  // namespace ce::capi
