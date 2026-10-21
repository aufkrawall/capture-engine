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

ce_status_t ResolvePackage(const std::optional<std::string>& packageDir, const std::optional<std::string>& settingsPath,
                           uint32_t features, PackageLayout& out, const char** reason) {
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

    // The package's executable: the role host, and the anchor the configuration and log paths derive from.
    const std::wstring packageExecutable = (std::filesystem::path(directory) / L"captureengine.exe").wstring();
    std::wstring roleHost = packageExecutable;
    if ((features & CE_FEATURE_SELF_ROLE_HOST) != 0) {
        // A helper process finds the package from the cengine.dll it loaded (common/platform/package_module.h).
        const std::wstring moduleDirectory =
            ce::ansi_path::ParentDirectoryW(ce::ansi_path::ModulePathW(out.windowModule));
        std::error_code ec;
        if (packageDir && !std::filesystem::equivalent(directory, moduleDirectory, ec)) {
            why = "CE_FEATURE_SELF_ROLE_HOST needs package_dir to be the folder of cengine.dll";
            return CE_E_INVALID_ARGUMENT;
        }
        roleHost = ce::ansi_path::ModulePathW(nullptr);
        if (roleHost.empty()) {
            why = "the client executable's path cannot be read";
            return CE_E_PACKAGE;
        }
    } else {
        const DWORD attributes = GetFileAttributesW(roleHost.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            LogWarn("[CApi] role host is missing: %s", ce::ansi_path::CompatiblePath(roleHost).c_str());
            why = "captureengine.exe is missing from the package directory";
            return CE_E_PACKAGE;
        }
    }

    ce::runtime::ConfigurationArgument configuration;
    if (settingsPath) {
        configuration.status = ce::runtime::ConfigurationArgumentStatus::Supplied;
        if (!Utf8ToWideStrict(*settingsPath, configuration.path)) {
            why = "settings_path is not valid UTF-8";
            return CE_E_INVALID_ARGUMENT;
        }
        if (!std::filesystem::path(configuration.path).is_absolute()) {
            why = "settings_path must be an absolute path";
            return CE_E_INVALID_ARGUMENT;
        }
    }
    // The ANSI configuration path the INI loader and the logs use, derived the way the executable derives it.
    ce::runtime::PackagePathError error = ce::runtime::PackagePathError::None;
    const auto paths = ce::runtime::RuntimePackagePaths::FromExecutable(packageExecutable, configuration, error);
    if (!paths) {
        why = "the package layout cannot be resolved";
        return CE_E_PACKAGE;
    }
    if (!paths->EncodingExact()) {
        LogWarn("[CApi] package path is not representable in the active code page: %s",
                paths->Configuration().c_str());
        why = "the package or settings path is not representable in the active code page";
        return CE_E_PACKAGE;
    }
    out.roleHost = roleHost;
    out.packageDir = ce::ansi_path::ParentDirectoryW(packageExecutable);
    out.configPath = paths->Configuration();
    out.configPathWide = paths->ConfigurationWide();
    out.logsRoot = paths->Directory() + "\\logs";
    return CE_OK;
}

}  // namespace ce::capi
