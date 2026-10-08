#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ce::runtime {
enum class ConfigurationArgumentStatus { Missing, Supplied, Invalid };
struct ConfigurationArgument {
    ConfigurationArgumentStatus status = ConfigurationArgumentStatus::Missing;
    std::wstring path;
};

// Takes parsed Windows arguments, including argv[0]. Only an exact option is
// eligible; duplicates and empty values fail rather than selecting another INI.
ConfigurationArgument ReadConfigurationArgument(const std::vector<std::wstring>& arguments);
ConfigurationArgument ReadProcessConfigurationArgument();

enum class PackagePathError { None, ModuleUnavailable, InvalidExecutable, InvalidConfiguration };

// Owns the runtime's executable/configuration locations. Defaults are anchored
// to that executable, not the caller's CWD. ANSI views preserve the executable's
// existing exact-code-page/8.3 behavior for INI, logs and crash handling.
class RuntimePackagePaths {
public:
    static std::optional<RuntimePackagePaths> FromExecutable(const std::wstring& executable,
                                                             const ConfigurationArgument& configuration,
                                                             PackagePathError& error);
    // An EXE preserves its own name; a DLL package uses captureengine.exe next
    // to the module. This permits a differently named embedding client.
    static std::optional<RuntimePackagePaths> FromModule(HMODULE module, const ConfigurationArgument& configuration,
                                                         PackagePathError& error);

    const std::wstring& Executable() const {
        return executable_;
    }
    const std::wstring& ConfigurationWide() const {
        return configurationWide_;
    }
    const std::string& Configuration() const {
        return configuration_;
    }
    const std::string& Directory() const {
        return directory_;
    }
    bool EncodingExact() const {
        return encodingExact_;
    }

private:
    RuntimePackagePaths() = default;
    std::wstring executable_;
    std::wstring configurationWide_;
    std::string configuration_;
    std::string directory_;
    bool encodingExact_ = false;
};

// Existing process-launch callers supply active-code-page paths, not UTF-8.
std::wstring ActiveCodePagePathToWide(std::string_view path);
}  // namespace ce::runtime
