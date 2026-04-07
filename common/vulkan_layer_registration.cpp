#include "vulkan_layer_registration.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <string>
#include <system_error>
#include <vector>

#include "logging.h"

namespace ce::vulkan_layer {
namespace {

constexpr wchar_t kImplicitLayersKey[] = L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers";
constexpr wchar_t kManifest64Name[] = L"VK_LAYER_CE_overlay.json";
constexpr wchar_t kLibrary64Name[] = L"VK_LAYER_CE_overlay.dll";
constexpr wchar_t kLayer64Name[] = L"VK_LAYER_CE_overlay";
constexpr wchar_t kManifest32Name[] = L"VK_LAYER_CE_overlay_x86.json";
constexpr wchar_t kLibrary32Name[] = L"VK_LAYER_CE_overlay_x86.dll";
constexpr wchar_t kLayer32Name[] = L"VK_LAYER_CE_overlay_x86";
constexpr wchar_t kLegacyManifestName[] = L"VK_LAYER_CAPTURE_overlay.json";

struct RegistryLocation {
    RegistryRoot root;
    RegistryView view;
};

class RegistryKeyGuard {
public:
    RegistryKeyGuard() = default;
    ~RegistryKeyGuard() {
        Reset();
    }

    RegistryKeyGuard(const RegistryKeyGuard&) = delete;
    RegistryKeyGuard& operator=(const RegistryKeyGuard&) = delete;

    RegistryKeyGuard(RegistryKeyGuard&& other) noexcept : key_(other.key_) {
        other.key_ = nullptr;
    }

    RegistryKeyGuard& operator=(RegistryKeyGuard&& other) noexcept {
        if (this != &other) {
            Reset();
            key_ = other.key_;
            other.key_ = nullptr;
        }
        return *this;
    }

    void Reset(HKEY key = nullptr) {
        if (key_) {
            RegCloseKey(key_);
        }
        key_ = key;
    }

    HKEY Get() const {
        return key_;
    }

    HKEY* Put() {
        Reset();
        return &key_;
    }

private:
    HKEY key_ = nullptr;
};

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

std::wstring NormalizePathForComparison(const std::filesystem::path& path) {
    std::wstring normalized = path.lexically_normal().wstring();
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    return ToLower(normalized);
}

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

class HandleCloser {
public:
    explicit HandleCloser(HANDLE handle) : handle_(handle) {}
    ~HandleCloser() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    HandleCloser(const HandleCloser&) = delete;
    HandleCloser& operator=(const HandleCloser&) = delete;

private:
    HANDLE handle_ = nullptr;
};

std::string FormatWindowsError(DWORD error) {
    char* message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageA(flags, nullptr, error, 0, reinterpret_cast<LPSTR>(&message), 0, nullptr);
    if (length == 0 || message == nullptr) {
        return std::to_string(error);
    }

    std::string result(message, length);
    LocalFree(message);

    while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || std::isspace(static_cast<unsigned char>(result.back())))) {
        result.pop_back();
    }
    return result;
}

bool IsRegularFile(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

LayerManifest BuildManifest(const std::filesystem::path& baseDir,
                            const wchar_t* manifestName,
                            const wchar_t* libraryName,
                            const wchar_t* layerName,
                            bool is32Bit) {
    LayerManifest manifest;
    manifest.manifestPath = baseDir / manifestName;
    manifest.libraryPath = baseDir / libraryName;
    manifest.layerName = layerName;
    manifest.is32Bit = is32Bit;
    manifest.manifestExists = IsRegularFile(manifest.manifestPath);
    manifest.libraryExists = IsRegularFile(manifest.libraryPath);
    return manifest;
}

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

std::string DescribeLocation(const RegistryLocation& location) {
    return std::string(ToString(location.root)) + "/" + ToString(location.view);
}

bool MatchesCurrentManifestPath(const RegistrationPlan& plan, const std::filesystem::path& path) {
    const std::wstring normalized = NormalizePathForComparison(path);
    for (const LayerManifest& manifest : plan.manifests) {
        if (NormalizePathForComparison(manifest.manifestPath) == normalized) {
            return true;
        }
    }
    return false;
}

bool TargetContainsManifestPath(const RegistrationPlan& plan,
                                RegistryRoot root,
                                RegistryView view,
                                const std::filesystem::path& path) {
    const std::wstring normalized = NormalizePathForComparison(path);
    for (const RegistryTarget& target : plan.installTargets) {
        if (target.root != root || target.view != view) {
            continue;
        }
        for (const LayerManifest& manifest : target.manifests) {
            if (NormalizePathForComparison(manifest.manifestPath) == normalized) {
                return true;
            }
        }
    }
    return false;
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

std::vector<std::wstring> EnumerateValueNames(HKEY key) {
    std::vector<std::wstring> valueNames;
    std::vector<wchar_t> buffer(32768, L'\0');
    DWORD index = 0;

    while (true) {
        DWORD valueNameLength = static_cast<DWORD>(buffer.size());
        LONG result = RegEnumValueW(key, index, buffer.data(), &valueNameLength, nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (result == ERROR_MORE_DATA) {
            buffer.resize(buffer.size() * 2, L'\0');
            continue;
        }
        if (result != ERROR_SUCCESS) {
            LogWarn("[VulkanReg] Failed to enumerate registry value %lu: %ld (%s)", index, result,
                    FormatWindowsError(result).c_str());
            break;
        }

        valueNames.emplace_back(buffer.data(), valueNameLength);
        ++index;
    }

    return valueNames;
}

std::vector<RegistryLocation> GetWritableCleanupLocations(const RegistrationPlan& plan) {
    std::vector<RegistryLocation> locations;
    locations.push_back({RegistryRoot::CurrentUser, RegistryView::Default});
    if (plan.processElevated) {
        locations.push_back({RegistryRoot::LocalMachine, RegistryView::Registry64});
        locations.push_back({RegistryRoot::LocalMachine, RegistryView::Registry32});
    }
    return locations;
}

std::vector<RegistryTarget> BuildStatusTargets(const RegistrationPlan& plan) {
    std::vector<RegistryTarget> targets;

    if (plan.effectiveMode == RegistrationMode::AllUsers) {
        RegistryTarget x64Target;
        x64Target.root = RegistryRoot::LocalMachine;
        x64Target.view = RegistryView::Registry64;
        RegistryTarget x86Target;
        x86Target.root = RegistryRoot::LocalMachine;
        x86Target.view = RegistryView::Registry32;

        for (const LayerManifest& manifest : plan.manifests) {
            if (manifest.is32Bit) {
                x86Target.manifests.push_back(manifest);
            } else {
                x64Target.manifests.push_back(manifest);
            }
        }

        if (!x64Target.manifests.empty()) {
            targets.push_back(std::move(x64Target));
        }
        if (!x86Target.manifests.empty()) {
            targets.push_back(std::move(x86Target));
        }
        return targets;
    }

    RegistryTarget currentUserTarget;
    currentUserTarget.root = RegistryRoot::CurrentUser;
    currentUserTarget.view = RegistryView::Default;
    currentUserTarget.manifests = plan.manifests;
    if (!currentUserTarget.manifests.empty()) {
        targets.push_back(std::move(currentUserTarget));
    }
    return targets;
}

bool DeleteRegistryValue(HKEY key, const std::wstring& valueName, const char* reason, const RegistryLocation& location) {
    const LONG result = RegDeleteValueW(key, valueName.c_str());
    if (result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND) {
        LogInfo("[VulkanReg] Removed %s entry from %s: %s", reason, DescribeLocation(location).c_str(),
                WideToUtf8(valueName).c_str());
        return true;
    }

    LogError("[VulkanReg] Failed to remove %s entry from %s: %s (error=%ld, %s)", reason,
             DescribeLocation(location).c_str(), WideToUtf8(valueName).c_str(), result,
             FormatWindowsError(result).c_str());
    return false;
}

bool CleanupRegistryLocation(const RegistrationPlan& plan, const RegistryLocation& location, bool removeCurrentManifests) {
    RegistryKeyGuard key;
    const LONG openResult = OpenRegistryKey(location, KEY_QUERY_VALUE | KEY_SET_VALUE, false, &key);
    if (openResult == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (openResult != ERROR_SUCCESS) {
        LogError("[VulkanReg] Failed to open %s for cleanup (error=%ld, %s)", DescribeLocation(location).c_str(),
                 openResult, FormatWindowsError(openResult).c_str());
        return false;
    }

    bool success = true;
    for (const std::wstring& valueName : EnumerateValueNames(key.Get())) {
        const std::filesystem::path valuePath(valueName);
        const bool isCurrentManifest = MatchesCurrentManifestPath(plan, valuePath);
        const bool manifestExists = IsRegularFile(valuePath);

        if (removeCurrentManifests && isCurrentManifest) {
            success &= DeleteRegistryValue(key.Get(), valueName, "current manifest", location);
            continue;
        }

        if (!ShouldDeleteRegistryValueForTarget(plan, location.root, location.view, valuePath, manifestExists)) {
            continue;
        }

        const bool intendedForTarget = TargetContainsManifestPath(plan, location.root, location.view, valuePath);
        const char* reason = (!manifestExists && !isCurrentManifest) ? "stale manifest"
                                                                     : (intendedForTarget ? "inactive manifest"
                                                                                          : "duplicate manifest");
        success &= DeleteRegistryValue(key.Get(), valueName, reason, location);
    }

    return success;
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

}  // namespace

const char* ToString(RegistrationMode mode) {
    switch (mode) {
        case RegistrationMode::Auto:
            return "auto";
        case RegistrationMode::CurrentUser:
            return "current-user";
        case RegistrationMode::AllUsers:
            return "all-users";
        default:
            return "unknown";
    }
}

const char* ToString(RegistryRoot root) {
    switch (root) {
        case RegistryRoot::CurrentUser:
            return "HKCU";
        case RegistryRoot::LocalMachine:
            return "HKLM";
        default:
            return "unknown";
    }
}

const char* ToString(RegistryView view) {
    switch (view) {
        case RegistryView::Default:
            return "shared";
        case RegistryView::Registry32:
            return "32-bit";
        case RegistryView::Registry64:
            return "64-bit";
        default:
            return "unknown";
    }
}

bool IsCurrentProcessElevated() {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        const DWORD error = GetLastError();
        LogWarn("[VulkanReg] OpenProcessToken failed while checking elevation: %lu (%s)", error,
                FormatWindowsError(error).c_str());
        return false;
    }

    HandleCloser token(rawToken);
    TOKEN_ELEVATION elevation = {};
    DWORD size = sizeof(elevation);
    if (!GetTokenInformation(rawToken, TokenElevation, &elevation, sizeof(elevation), &size)) {
        const DWORD error = GetLastError();
        LogWarn("[VulkanReg] GetTokenInformation(TokenElevation) failed: %lu (%s)", error,
                FormatWindowsError(error).c_str());
        return false;
    }

    return elevation.TokenIsElevated != 0;
}

bool GetCurrentExecutableDirectory(std::filesystem::path* outDir) {
    if (!outDir) {
        return false;
    }

    std::vector<wchar_t> buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            const DWORD error = GetLastError();
            LogError("[VulkanReg] GetModuleFileNameW failed: %lu (%s)", error, FormatWindowsError(error).c_str());
            return false;
        }

        if (length < buffer.size() - 1) {
            *outDir = std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
            return true;
        }

        buffer.resize(buffer.size() * 2, L'\0');
    }
}

RegistrationPlan BuildRegistrationPlan(const std::filesystem::path& baseDir,
                                       RegistrationMode requestedMode,
                                       bool processElevated) {
    RegistrationPlan plan;
    plan.baseDir = baseDir;
    plan.requestedMode = requestedMode;
    plan.processElevated = processElevated;
    plan.effectiveMode = requestedMode;
    if (requestedMode == RegistrationMode::Auto) {
        plan.effectiveMode = processElevated ? RegistrationMode::AllUsers : RegistrationMode::CurrentUser;
    }

    plan.manifests.push_back(BuildManifest(baseDir, kManifest64Name, kLibrary64Name, kLayer64Name, false));
    plan.manifests.push_back(BuildManifest(baseDir, kManifest32Name, kLibrary32Name, kLayer32Name, true));

    if (plan.effectiveMode == RegistrationMode::AllUsers) {
        RegistryTarget x64Target;
        x64Target.root = RegistryRoot::LocalMachine;
        x64Target.view = RegistryView::Registry64;
        RegistryTarget x86Target;
        x86Target.root = RegistryRoot::LocalMachine;
        x86Target.view = RegistryView::Registry32;

        for (const LayerManifest& manifest : plan.manifests) {
            if (!manifest.IsUsable()) {
                continue;
            }
            if (manifest.is32Bit) {
                x86Target.manifests.push_back(manifest);
            } else {
                x64Target.manifests.push_back(manifest);
            }
        }

        if (!x64Target.manifests.empty()) {
            plan.installTargets.push_back(std::move(x64Target));
        }
        if (!x86Target.manifests.empty()) {
            plan.installTargets.push_back(std::move(x86Target));
        }
        return plan;
    }

    RegistryTarget currentUserTarget;
    currentUserTarget.root = RegistryRoot::CurrentUser;
    currentUserTarget.view = RegistryView::Default;
    for (const LayerManifest& manifest : plan.manifests) {
        if (manifest.IsUsable()) {
            currentUserTarget.manifests.push_back(manifest);
        }
    }

    if (!currentUserTarget.manifests.empty()) {
        plan.installTargets.push_back(std::move(currentUserTarget));
    }
    return plan;
}

bool IsCaptureEngineLayerManifestPath(const std::filesystem::path& path) {
    const std::wstring fileName = ToLower(path.filename().wstring());
    return fileName == ToLower(kManifest64Name) || fileName == ToLower(kManifest32Name) ||
           fileName == ToLower(kLegacyManifestName);
}

std::string PathToUtf8ForLogging(const std::filesystem::path& path) {
    return PathToUtf8(path);
}

bool ShouldDeleteRegistryValueForTarget(const RegistrationPlan& plan,
                                        RegistryRoot root,
                                        RegistryView view,
                                        const std::filesystem::path& valuePath,
                                        bool manifestExists) {
    if (!IsCaptureEngineLayerManifestPath(valuePath)) {
        return false;
    }

    if (!TargetContainsManifestPath(plan, root, view, valuePath)) {
        return true;
    }

    return !manifestExists;
}

void LogRegistrationPlan(const RegistrationPlan& plan) {
    LogInfo("[VulkanReg] Registration mode: requested=%s effective=%s elevated=%s baseDir=%s",
            ToString(plan.requestedMode), ToString(plan.effectiveMode), plan.processElevated ? "true" : "false",
            PathToUtf8(plan.baseDir).c_str());

    if (plan.effectiveMode == RegistrationMode::CurrentUser) {
        LogInfo("[VulkanReg] Using HKCU registration. Elevated Vulkan apps will ignore per-user implicit layers.");
    } else {
        LogInfo("[VulkanReg] Using HKLM registration because this process is elevated or all-users registration was requested.");
    }

    if (plan.effectiveMode == RegistrationMode::AllUsers && !plan.processElevated) {
        LogWarn("[VulkanReg] All-users registration was requested without an elevated process. HKLM writes may fail.");
    }

    for (const LayerManifest& manifest : plan.manifests) {
        LogInfo("[VulkanReg] Manifest %s: json=%s dll=%s usable=%s", PathToUtf8(manifest.manifestPath).c_str(),
                manifest.manifestExists ? "present" : "missing", manifest.libraryExists ? "present" : "missing",
                manifest.IsUsable() ? "true" : "false");
    }

    if (plan.installTargets.empty()) {
        LogWarn("[VulkanReg] No usable Vulkan layer manifests were found for registration.");
        return;
    }

    for (const RegistryTarget& target : plan.installTargets) {
        LogInfo("[VulkanReg] Target %s/%s will register %zu manifest(s)", ToString(target.root), ToString(target.view),
                target.manifests.size());
        for (const LayerManifest& manifest : target.manifests) {
            LogInfo("[VulkanReg]   %s", PathToUtf8(manifest.manifestPath).c_str());
        }
    }
}

bool ApplyRegistrationPlan(const RegistrationPlan& plan, bool install) {
    bool success = true;
    for (const RegistryLocation& location : GetWritableCleanupLocations(plan)) {
        success &= CleanupRegistryLocation(plan, location, !install);
    }

    if (!install) {
        return success;
    }

    if (plan.installTargets.empty()) {
        LogWarn("[VulkanReg] Skipping Vulkan layer registration because there are no usable manifests.");
        return false;
    }

    for (const RegistryTarget& target : plan.installTargets) {
        success &= WriteRegistryTarget(target);
    }
    return success;
}

bool IsRegistrationActive(const RegistrationPlan& plan) {
    for (const RegistryTarget& target : BuildStatusTargets(plan)) {
        RegistryKeyGuard key;
        const RegistryLocation location{target.root, target.view};
        const LONG openResult = OpenRegistryKey(location, KEY_QUERY_VALUE, false, &key);
        if (openResult != ERROR_SUCCESS) {
            continue;
        }

        for (const LayerManifest& manifest : target.manifests) {
            const std::wstring valueName = manifest.manifestPath.wstring();
            LONG queryResult = RegQueryValueExW(key.Get(), valueName.c_str(), nullptr, nullptr, nullptr, nullptr);
            if (queryResult == ERROR_SUCCESS) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace ce::vulkan_layer
