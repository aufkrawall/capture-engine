#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ce::vulkan_layer {

enum class RegistrationMode {
    Auto,
    CurrentUser,
    AllUsers,
};

enum class RegistryRoot {
    CurrentUser,
    LocalMachine,
};

enum class RegistryView {
    Default,
    Registry32,
    Registry64,
};

struct LayerManifest {
    std::filesystem::path manifestPath;
    std::filesystem::path libraryPath;
    std::wstring layerName;
    bool is32Bit = false;
    bool manifestExists = false;
    bool libraryExists = false;

    bool IsUsable() const {
        return manifestExists && libraryExists;
    }
};

struct RegistryTarget {
    RegistryRoot root = RegistryRoot::CurrentUser;
    RegistryView view = RegistryView::Default;
    std::vector<LayerManifest> manifests;
};

struct RegistrationPlan {
    std::filesystem::path baseDir;
    RegistrationMode requestedMode = RegistrationMode::Auto;
    RegistrationMode effectiveMode = RegistrationMode::CurrentUser;
    bool processElevated = false;
    std::vector<LayerManifest> manifests;
    std::vector<RegistryTarget> installTargets;
};

const char* ToString(RegistrationMode mode);
const char* ToString(RegistryRoot root);
const char* ToString(RegistryView view);

bool IsCurrentProcessElevated();
bool GetCurrentExecutableDirectory(std::filesystem::path* outDir);

RegistrationPlan BuildRegistrationPlan(const std::filesystem::path& baseDir, RegistrationMode requestedMode,
                                       bool processElevated);

std::string PathToUtf8ForLogging(const std::filesystem::path& path);

void LogRegistrationPlan(const RegistrationPlan& plan);
bool RepairOwnedRegistrations(const RegistrationPlan& plan);
bool ApplyRegistrationPlan(const RegistrationPlan& plan, bool install);
bool IsRegistrationActive(const RegistrationPlan& plan);

}  // namespace ce::vulkan_layer
