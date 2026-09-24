#include "vulkan_layer_registration_registry.h"

#include <cctype>
#include <string>
#include <system_error>
#include <vector>

#include "logging.h"

namespace ce::vulkan_layer {
namespace {

REGSAM GetViewFlags(RegistryView view) {
    switch (view) {
        case RegistryView::Registry32:
            return KEY_WOW64_32KEY;
        case RegistryView::Registry64:
            return KEY_WOW64_64KEY;
        case RegistryView::Default:
        default:
            return 0;
    }
}

HKEY GetRootHandle(RegistryRoot root) {
    return root == RegistryRoot::LocalMachine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

}  // namespace

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }

    std::string result(required, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), required, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::string PathToUtf8(const std::filesystem::path& path) {
    return WideToUtf8(path.wstring());
}

std::string FormatWindowsError(DWORD error) {
    char* message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageA(flags, nullptr, error, 0, reinterpret_cast<LPSTR>(&message), 0, nullptr);
    if (length == 0 || message == nullptr) {
        return std::to_string(error);
    }

    std::string result(message, length);
    LocalFree(message);

    while (!result.empty() && (result.back() == '\r' || result.back() == '\n' ||
                               std::isspace(static_cast<unsigned char>(result.back())))) {
        result.pop_back();
    }
    return result;
}

std::string DescribeLocation(const RegistryLocation& location) {
    return std::string(ToString(location.root)) + "/" + ToString(location.view);
}

LONG OpenRegistryKey(const RegistryLocation& location, REGSAM access, bool create, RegistryKeyGuard* outKey) {
    HKEY rawKey = nullptr;
    const REGSAM sam = access | GetViewFlags(location.view);
    LONG result = ERROR_SUCCESS;

    if (create) {
        result = RegCreateKeyExW(GetRootHandle(location.root), kImplicitLayersKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                 sam, nullptr, &rawKey, nullptr);
    } else {
        result = RegOpenKeyExW(GetRootHandle(location.root), kImplicitLayersKey, 0, sam, &rawKey);
    }

    if (result == ERROR_SUCCESS) {
        outKey->Reset(rawKey);
    }
    return result;
}

bool DeleteRegistryValue(HKEY key, const std::wstring& valueName, const char* reason,
                         const RegistryLocation& location) {
    const LONG result = RegDeleteValueW(key, valueName.c_str());
    if (result == ERROR_SUCCESS) {
        LogInfo("[VulkanReg] Removed %s entry from %s: %s", reason, DescribeLocation(location).c_str(),
                WideToUtf8(valueName).c_str());
        return true;
    }
    if (result == ERROR_FILE_NOT_FOUND) {
        return true;
    }

    LogError("[VulkanReg] Failed to remove %s entry from %s: %s (error=%ld, %s)", reason,
             DescribeLocation(location).c_str(), WideToUtf8(valueName).c_str(), result,
             FormatWindowsError(result).c_str());
    return false;
}

bool WriteRegistryTarget(const RegistryTarget& target) {
    if (target.manifests.empty()) {
        return true;
    }

    RegistryKeyGuard key;
    const RegistryLocation location{target.root, target.view};
    const LONG openResult = OpenRegistryKey(location, KEY_SET_VALUE, true, &key);
    if (openResult != ERROR_SUCCESS) {
        LogError("[VulkanReg] Failed to open %s for registration (error=%ld, %s)", DescribeLocation(location).c_str(),
                 openResult, FormatWindowsError(openResult).c_str());
        return false;
    }

    bool success = true;
    for (const LayerManifest& manifest : target.manifests) {
        const DWORD enabled = 0;
        const std::wstring valueName = manifest.manifestPath.wstring();
        const LONG setResult = RegSetValueExW(key.Get(), valueName.c_str(), 0, REG_DWORD,
                                              reinterpret_cast<const BYTE*>(&enabled), sizeof(enabled));
        if (setResult == ERROR_SUCCESS) {
            LogInfo("[VulkanReg] Registered %s in %s", PathToUtf8(manifest.manifestPath).c_str(),
                    DescribeLocation(location).c_str());
        } else {
            LogError("[VulkanReg] Failed to register %s in %s (error=%ld, %s)",
                     PathToUtf8(manifest.manifestPath).c_str(), DescribeLocation(location).c_str(), setResult,
                     FormatWindowsError(setResult).c_str());
            success = false;
        }
    }

    return success;
}

}  // namespace ce::vulkan_layer
