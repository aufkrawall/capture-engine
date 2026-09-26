#include "vulkan_layer_registration.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "build_identity.h"
#include "logging.h"
#include "vulkan_layer_registration_registry.h"

namespace ce::vulkan_layer {
namespace {

constexpr wchar_t kManifest64Name[] = L"VK_LAYER_CE_overlay.json";
constexpr wchar_t kLibrary64Name[] = L"VK_LAYER_CE_overlay.dll";
constexpr wchar_t kLayer64Name[] = L"VK_LAYER_CE_overlay";
constexpr wchar_t kManifest32Name[] = L"VK_LAYER_CE_overlay_x86.json";
constexpr wchar_t kLibrary32Name[] = L"VK_LAYER_CE_overlay_x86.dll";
constexpr wchar_t kLayer32Name[] = L"VK_LAYER_CE_overlay_x86";
constexpr wchar_t kGate64Name[] = L"VK_LAYER_CE_gate.dll";
constexpr wchar_t kGate32Name[] = L"VK_LAYER_CE_gate_x86.dll";
constexpr wchar_t kLegacyManifestName[] = L"VK_LAYER_CAPTURE_overlay.json";
constexpr wchar_t kStagingSubdirectory[] = L"CaptureEngine\\vulkan_layers";

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

bool IsOwnedManifestPath(const std::filesystem::path& path) {
    const std::wstring fileName = ToLower(path.filename().wstring());
    return fileName == ToLower(kManifest64Name) || fileName == ToLower(kManifest32Name) ||
           fileName == ToLower(kLegacyManifestName);
}

std::wstring BuildVersionedLayerName(const wchar_t* baseName) {
    return std::wstring(baseName) + L"_b" + std::to_wstring(GetCurrentBuildNumber());
}

class HandleCloser {
public:
    explicit HandleCloser(HANDLE handle) : handle_(handle) {}
    ~HandleCloser() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    HandleCloser(const HandleCloser&) = delete;
    HandleCloser& operator=(const HandleCloser&) = delete;
private:
    HANDLE handle_ = nullptr;
};

bool IsRegularFile(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

LayerManifest BuildManifest(const std::filesystem::path& baseDir, const std::filesystem::path& stagingDir,
                            const wchar_t* manifestName, const wchar_t* libraryName, const wchar_t* gateName,
                            const wchar_t* layerName, bool is32Bit) {
    LayerManifest manifest;
    manifest.sourceManifestPath = baseDir / manifestName;
    manifest.sourceLibraryPath = baseDir / libraryName;
    manifest.sourceGatePath = baseDir / gateName;
    manifest.manifestPath = stagingDir / manifestName;
    manifest.libraryPath = stagingDir / libraryName;
    manifest.gatePath = stagingDir / gateName;
    manifest.layerName = BuildVersionedLayerName(layerName);
    manifest.is32Bit = is32Bit;
    manifest.libraryExists = IsRegularFile(manifest.sourceLibraryPath);
    manifest.gateExists = IsRegularFile(manifest.sourceGatePath);
    manifest.manifestExists = manifest.libraryExists || IsRegularFile(manifest.sourceManifestPath);
    return manifest;
}

std::vector<std::wstring> EnumerateRegistryValueNames(HKEY key) {
    std::vector<std::wstring> names;
    std::vector<wchar_t> buffer(512, L'\0');
    DWORD index = 0;

    while (true) {
        DWORD length = static_cast<DWORD>(buffer.size());
        const LONG result = RegEnumValueW(key, index, buffer.data(), &length, nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (result == ERROR_MORE_DATA) {
            buffer.resize(buffer.size() * 2, L'\0');
            continue;
        }
        if (result != ERROR_SUCCESS) {
            LogWarn("[VulkanReg] Failed to enumerate %s value %lu (error=%ld, %s)",
                    WideToUtf8(kImplicitLayersKey).c_str(), index, result, FormatWindowsError(result).c_str());
            break;
        }

        names.emplace_back(buffer.data(), length);
        ++index;
    }
    return names;
}

// WOW64 redirects HKLM\Software (to Wow6432Node) but shares HKCU\Software, so
// KEY_WOW64_32KEY and KEY_WOW64_64KEY open the same HKCU ImplicitLayers key.
bool RootSharesRegistryViews(RegistryRoot root) {
    return root == RegistryRoot::CurrentUser;
}

std::vector<RegistryTarget> BuildStatusTargets(const RegistrationPlan& plan) {
    std::vector<RegistryTarget> targets;
    const RegistryRoot root =
        (plan.effectiveMode == RegistrationMode::AllUsers) ? RegistryRoot::LocalMachine : RegistryRoot::CurrentUser;
    RegistryTarget x64Target{root, RegistryView::Registry64, {}};
    RegistryTarget x86Target{root, RegistryView::Registry32, {}};
    for (const LayerManifest& manifest : plan.manifests) {
        (manifest.is32Bit ? x86Target : x64Target).manifests.push_back(manifest);
    }
    if (!x64Target.manifests.empty()) targets.push_back(std::move(x64Target));
    if (!x86Target.manifests.empty()) targets.push_back(std::move(x86Target));
    return targets;
}

bool DeleteRegistryTarget(const RegistryTarget& target) {
    if (target.manifests.empty()) {
        return true;
    }

    const RegistryLocation location{target.root, target.view};
    RegistryKeyGuard key;
    const LONG openResult = OpenRegistryKey(location, KEY_SET_VALUE, false, &key);
    if (openResult == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (openResult != ERROR_SUCCESS) {
        LogError("[VulkanReg] Failed to open %s for unregistration (error=%ld, %s)", DescribeLocation(location).c_str(),
                 openResult, FormatWindowsError(openResult).c_str());
        return false;
    }

    bool success = true;
    for (const LayerManifest& manifest : target.manifests) {
        success &= DeleteRegistryValue(key.Get(), manifest.manifestPath.wstring(), "owned manifest", location);
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

bool ResolveDefaultStagingDirectory(RegistrationMode mode, std::filesystem::path* outDir) {
    if (!outDir) {
        return false;
    }

    std::vector<wchar_t> buffer(MAX_PATH, L'\0');
    DWORD written = 0;
    if (mode == RegistrationMode::AllUsers) {
        written = GetEnvironmentVariableW(L"ProgramData", buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0 || written >= buffer.size()) {
            written = GetEnvironmentVariableW(L"ALLUSERSPROFILE", buffer.data(), static_cast<DWORD>(buffer.size()));
        }
    } else {
        written = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    }

    if (written == 0 || written >= buffer.size()) {
        LogWarn("[VulkanReg] Failed to resolve environment root for staging mode %s", ToString(mode));
        return false;
    }

    const std::wstring buildFolder = L"b" + std::to_wstring(GetCurrentBuildNumber());
    *outDir = std::filesystem::path(std::wstring(buffer.data(), written)) / kStagingSubdirectory / buildFolder;
    return true;
}

RegistrationPlan BuildRegistrationPlan(const std::filesystem::path& baseDir, RegistrationMode requestedMode,
                                       bool processElevated, const std::filesystem::path& explicitStagingDir) {
    RegistrationPlan plan;
    plan.baseDir = baseDir;
    plan.requestedMode = requestedMode;
    plan.processElevated = processElevated;
    plan.effectiveMode = requestedMode;
    if (requestedMode == RegistrationMode::Auto) {
        plan.effectiveMode = processElevated ? RegistrationMode::AllUsers : RegistrationMode::CurrentUser;
    }

    if (!explicitStagingDir.empty()) {
        plan.stagingDir = explicitStagingDir;
    } else if (!ResolveDefaultStagingDirectory(plan.effectiveMode, &plan.stagingDir)) {
        LogWarn("[VulkanReg] Could not resolve default staging directory; falling back to source directory %s",
                PathToUtf8(baseDir).c_str());
        plan.stagingDir = baseDir;
    }

    plan.manifests.push_back(
        BuildManifest(baseDir, plan.stagingDir, kManifest64Name, kLibrary64Name, kGate64Name, kLayer64Name, false));
    plan.manifests.push_back(
        BuildManifest(baseDir, plan.stagingDir, kManifest32Name, kLibrary32Name, kGate32Name, kLayer32Name, true));

    const RegistryRoot root =
        (plan.effectiveMode == RegistrationMode::AllUsers) ? RegistryRoot::LocalMachine : RegistryRoot::CurrentUser;
    RegistryTarget x64Target{root, RegistryView::Registry64, {}};
    RegistryTarget x86Target{root, RegistryView::Registry32, {}};
    for (const LayerManifest& manifest : plan.manifests) {
        if (manifest.IsUsable()) {
            (manifest.is32Bit ? x86Target : x64Target).manifests.push_back(manifest);
        }
    }
    if (!x64Target.manifests.empty()) plan.installTargets.push_back(std::move(x64Target));
    if (!x86Target.manifests.empty()) plan.installTargets.push_back(std::move(x86Target));
    return plan;
}

std::string PathToUtf8ForLogging(const std::filesystem::path& path) {
    return PathToUtf8(path);
}

std::vector<std::wstring> SelectStaleOwnedEntries(const std::vector<std::wstring>& existingValueNames,
                                                  const std::vector<std::wstring>& retainedValueNames) {
    std::vector<std::wstring> retainedLower;
    retainedLower.reserve(retainedValueNames.size());
    for (const std::wstring& retained : retainedValueNames) {
        retainedLower.push_back(ToLower(retained));
    }

    std::vector<std::wstring> stale;
    for (const std::wstring& existing : existingValueNames) {
        // Only CE's own manifests are ever eligible. A foreign implicit layer
        // must survive untouched even when it sits in the same registry key.
        if (!IsOwnedManifestPath(std::filesystem::path(existing))) {
            continue;
        }
        const std::wstring existingLower = ToLower(existing);
        if (std::find(retainedLower.begin(), retainedLower.end(), existingLower) != retainedLower.end()) {
            continue;
        }
        stale.push_back(existing);
    }
    return stale;
}

// The retained names are the exact values this instance keeps registered in each
// physical key. Pruning everything else (instead of deleting all owned entries
// and rewriting them) keeps the live registration continuously present: the
// Vulkan loader reads ImplicitLayers inside vkCreateInstance, so a delete/rewrite
// window would drop the layer from any title that happened to start during it.
// Treating the two HKCU views as separate keys did exactly that on every start:
// each view's pass classified the other architecture's live entry as a
// wrong-view leftover and deleted it, and both stayed absent until
// ApplyRegistrationPlan had staged the artifacts and rewritten them (~18 ms in
// session logs/20260926_044427).
std::vector<RepairScope> BuildRepairScopes(const RegistrationPlan& plan) {
    std::vector<RepairScope> scopes = {{RegistryRoot::CurrentUser, RegistryView::Default, {}}};
    if (plan.processElevated) {
        scopes.push_back({RegistryRoot::LocalMachine, RegistryView::Registry64, {}});
        scopes.push_back({RegistryRoot::LocalMachine, RegistryView::Registry32, {}});
    }
    for (RepairScope& scope : scopes) {
        for (const RegistryTarget& target : plan.installTargets) {
            if (target.root != scope.root) {
                continue;
            }
            if (!RootSharesRegistryViews(scope.root) && target.view != scope.view) {
                continue;
            }
            for (const LayerManifest& manifest : target.manifests) {
                scope.retainedValueNames.push_back(manifest.manifestPath.wstring());
            }
        }
    }
    return scopes;
}

bool RepairOwnedRegistrations(const RegistrationPlan& plan) {
    bool success = true;
    for (const RepairScope& scope : BuildRepairScopes(plan)) {
        const RegistryLocation location{scope.root, scope.view};
        RegistryKeyGuard key;
        const LONG openResult = OpenRegistryKey(location, KEY_QUERY_VALUE | KEY_SET_VALUE, false, &key);
        if (openResult == ERROR_FILE_NOT_FOUND) {
            continue;
        }
        if (openResult != ERROR_SUCCESS) {
            LogError("[VulkanReg] Failed to open %s for owned-entry repair (error=%ld, %s)",
                     DescribeLocation(location).c_str(), openResult, FormatWindowsError(openResult).c_str());
            success = false;
            continue;
        }

        const std::vector<std::wstring> stale =
            SelectStaleOwnedEntries(EnumerateRegistryValueNames(key.Get()), scope.retainedValueNames);
        LogInfo("[VulkanReg] Owned-entry repair %s: retaining %zu live CE entr%s, pruning %zu",
                DescribeLocation(location).c_str(), scope.retainedValueNames.size(),
                scope.retainedValueNames.size() == 1 ? "y" : "ies", stale.size());
        for (const std::wstring& valueName : stale) {
            success &= DeleteRegistryValue(key.Get(), valueName, "superseded CE manifest", location);
        }
    }

    if (!plan.processElevated) {
        const std::vector<RegistryLocation> hklmLocations = {
            {RegistryRoot::LocalMachine, RegistryView::Registry64},
            {RegistryRoot::LocalMachine, RegistryView::Registry32},
        };
        for (const auto& location : hklmLocations) {
            RegistryKeyGuard key;
            if (OpenRegistryKey(location, KEY_QUERY_VALUE, false, &key) == ERROR_SUCCESS) {
                for (const std::wstring& valueName :
                     SelectStaleOwnedEntries(EnumerateRegistryValueNames(key.Get()), {})) {
                    LogWarn(
                        "[VulkanReg] Stale CaptureEngine layer in %s: %s (requires elevation to repair; may shadow HKCU "
                        "staging)",
                        DescribeLocation(location).c_str(), WideToUtf8(valueName).c_str());
                }
            }
        }
    }

    return success;
}

static bool StageFileIfChanged(const std::filesystem::path& source, const std::filesystem::path& target) {
    if (source == target) {
        return true;
    }
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        const auto srcSize = std::filesystem::file_size(source, ec);
        const auto dstSize = std::filesystem::file_size(target, ec);
        if (!ec && srcSize == dstSize) {
            const auto srcTime = std::filesystem::last_write_time(source, ec);
            const auto dstTime = std::filesystem::last_write_time(target, ec);
            if (!ec && srcTime == dstTime) {
                return true;
            }
        }
    }

    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec) {
        LogInfo("[VulkanReg] Staged %s -> %s", PathToUtf8(source).c_str(), PathToUtf8(target).c_str());
        return true;
    }

    if (std::filesystem::exists(target, ec)) {
        LogWarn("[VulkanReg] Staged target %s in use; reusing existing image", PathToUtf8(target).c_str());
        return true;
    }

    LogError("[VulkanReg] Failed to stage %s to %s (error=%d, %s)", PathToUtf8(source).c_str(),
             PathToUtf8(target).c_str(), ec.value(), ec.message().c_str());
    return false;
}

static bool WriteStagedManifest(const LayerManifest& manifest) {
    if (manifest.manifestPath.empty()) {
        return false;
    }

    const std::string buildStr = std::to_string(GetCurrentBuildNumber());
    const std::string jsonContent =
        "{\n"
        "    \"file_format_version\": \"1.2.0\",\n"
        "    \"layer\": {\n"
        "        \"name\": \"" + WideToUtf8(manifest.layerName) + "\",\n"
        "        \"type\": \"GLOBAL\",\n"
        // The gate, never the full layer: the loader maps this library into
        // every Vulkan process. Its only entry point is negotiation; the proc
        // addresses come from the full layer through the negotiated struct.
        "        \"library_path\": \".\\\\" + WideToUtf8(manifest.gatePath.filename().wstring()) + "\",\n"
        "        \"api_version\": \"1.3.0\",\n"
        "        \"implementation_version\": \"" + buildStr + "\",\n"
        "        \"description\": \"CaptureEngine Overlay and Recording Layer\",\n"
        "        \"functions\": {\n"
        "            \"vkNegotiateLoaderLayerInterfaceVersion\": \"vkNegotiateLoaderLayerInterfaceVersion\"\n"
        "        },\n"
        "        \"disable_environment\": {\n"
        "            \"DISABLE_CE_VULKAN_LAYER\": \"1\"\n"
        "        }\n"
        "    }\n"
        "}\n";

    std::error_code ec;
    if (std::filesystem::exists(manifest.manifestPath, ec)) {
        std::ifstream in(manifest.manifestPath, std::ios::binary);
        if (in) {
            std::string existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (existing == jsonContent) {
                return true;
            }
        }
    }

    std::ofstream out(manifest.manifestPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        LogError("[VulkanReg] Failed to write staged manifest to %s", PathToUtf8(manifest.manifestPath).c_str());
        return false;
    }
    out.write(jsonContent.data(), static_cast<std::streamsize>(jsonContent.size()));
    LogInfo("[VulkanReg] Staged manifest: %s", PathToUtf8(manifest.manifestPath).c_str());
    return true;
}

static bool StagePlanArtifacts(const RegistrationPlan& plan) {
    if (plan.stagingDir.empty()) {
        return true;
    }

    // With stagingDir == baseDir - the env-root fallback BuildRegistrationPlan
    // falls back to - the generated manifest still has to be written, right
    // over the checked-in VK_LAYER_CE_overlay.json if one is present there:
    // that manifest names the FULL layer, and registering it maps the 1.5 MB
    // layer and its imports into every Vulkan process on the machine again,
    // which is the exposure the negotiation gate exists to remove. The
    // generated manifest is also the only one this path gets: nothing else
    // writes one when no staging directory is in play.
    bool success = true;
    if (plan.stagingDir != plan.baseDir) {
        std::error_code ec;
        std::filesystem::create_directories(plan.stagingDir, ec);
        if (ec) {
            LogError("[VulkanReg] Failed to create staging directory %s (error=%d, %s)",
                     PathToUtf8(plan.stagingDir).c_str(), ec.value(), ec.message().c_str());
            return false;
        }
    }

    for (const auto& manifest : plan.manifests) {
        if (!manifest.IsUsable()) {
            continue;
        }
        // The full layer first: the manifest names the gate, and a gate staged
        // without its full layer beside it could only decline.
        success &= StageFileIfChanged(manifest.sourceLibraryPath, manifest.libraryPath);
        success &= StageFileIfChanged(manifest.sourceGatePath, manifest.gatePath);
        success &= WriteStagedManifest(manifest);
    }
    return success;
}

void LogRegistrationPlan(const RegistrationPlan& plan) {
    LogInfo("[VulkanReg] Registration mode: requested=%s effective=%s elevated=%s baseDir=%s stagingDir=%s",
            ToString(plan.requestedMode), ToString(plan.effectiveMode), plan.processElevated ? "true" : "false",
            PathToUtf8(plan.baseDir).c_str(), PathToUtf8(plan.stagingDir).c_str());

    if (plan.effectiveMode == RegistrationMode::CurrentUser) {
        LogInfo("[VulkanReg] Using HKCU registration. Elevated Vulkan apps will ignore per-user implicit layers.");
    } else {
        LogInfo(
            "[VulkanReg] Using HKLM registration because this process is elevated or all-users registration was "
            "requested.");
    }

    if (plan.effectiveMode == RegistrationMode::AllUsers && !plan.processElevated) {
        LogWarn("[VulkanReg] All-users registration was requested without an elevated process. HKLM writes may fail.");
    }

    for (const LayerManifest& manifest : plan.manifests) {
        LogInfo("[VulkanReg] Manifest %s: json=%s dll=%s gate=%s usable=%s", PathToUtf8(manifest.manifestPath).c_str(),
                manifest.manifestExists ? "present" : "missing", manifest.libraryExists ? "present" : "missing",
                manifest.gateExists ? "present" : "missing", manifest.IsUsable() ? "true" : "false");
        LogInfo("[VulkanReg]   layer identity: %s", WideToUtf8(manifest.layerName).c_str());
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

bool CleanupStaleStagingDirectories(const RegistrationPlan& plan) {
    if (plan.stagingDir.empty() || plan.stagingDir == plan.baseDir) {
        return true;
    }

    const std::filesystem::path parentDir = plan.stagingDir.parent_path();
    std::error_code ec;
    if (!std::filesystem::exists(parentDir, ec) || !std::filesystem::is_directory(parentDir, ec)) {
        return true;
    }

    const std::wstring currentFolder = ToLower(plan.stagingDir.filename().wstring());
    for (const auto& entry : std::filesystem::directory_iterator(parentDir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_directory(ec)) {
            continue;
        }
        const std::wstring folderName = ToLower(entry.path().filename().wstring());
        if (folderName.rfind(L'b', 0) == 0 && folderName != currentFolder) {
            std::error_code rmEc;
            std::filesystem::remove_all(entry.path(), rmEc);
            if (!rmEc) {
                LogInfo("[VulkanReg] Pruned superseded staging directory: %s", PathToUtf8(entry.path()).c_str());
            } else {
                LogInfo("[VulkanReg] Retaining in-use superseded staging directory: %s",
                        PathToUtf8(entry.path()).c_str());
            }
        }
    }
    return true;
}

bool ApplyRegistrationPlan(const RegistrationPlan& plan, bool install) {
    if (install) {
        if (plan.installTargets.empty()) {
            LogWarn("[VulkanReg] Skipping Vulkan layer registration because there are no usable manifests.");
            return false;
        }

        if (!StagePlanArtifacts(plan)) {
            LogError("[VulkanReg] Failed to stage Vulkan layer artifacts to %s", PathToUtf8(plan.stagingDir).c_str());
            return false;
        }

        if (!plan.stagingDir.empty() && plan.stagingDir != plan.baseDir) {
            std::error_code rmEc;
            std::filesystem::remove(plan.baseDir / kManifest64Name, rmEc);
            std::filesystem::remove(plan.baseDir / kManifest32Name, rmEc);
        }

        bool success = true;
        for (const RegistryTarget& target : plan.installTargets) {
            success &= WriteRegistryTarget(target);
        }
        CleanupStaleStagingDirectories(plan);
        return success;
    }

    bool success = true;
    for (const RegistryTarget& target : plan.installTargets) {
        success &= DeleteRegistryTarget(target);
    }
    if (!plan.stagingDir.empty() && plan.stagingDir != plan.baseDir) {
        std::error_code ec;
        std::filesystem::remove_all(plan.stagingDir, ec);
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
