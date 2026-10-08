#include "runtime_package_paths.h"
#include "ansi_path.h"

#include <shellapi.h>
#include <filesystem>
#include <limits>

namespace ce::runtime {
namespace {
bool HasEmbeddedNull(const std::wstring& value) {
    return value.find(L'\0') != std::wstring::npos;
}

std::optional<std::wstring> AbsoluteConfiguration(const std::wstring& path) {
    if (path.empty() || HasEmbeddedNull(path))
        return std::nullopt;
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!required)
        return std::nullopt;
    std::wstring absolute(required, L'\0');
    const DWORD length = GetFullPathNameW(path.c_str(), required, absolute.data(), nullptr);
    if (!length || length >= required)
        return std::nullopt;
    absolute.resize(length);
    return absolute;
}
}  // namespace

ConfigurationArgument ReadConfigurationArgument(const std::vector<std::wstring>& arguments) {
    ConfigurationArgument result;
    constexpr std::wstring_view prefix = L"--config=";
    for (size_t index = 1; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        // --launch owns the remaining command line, including the game's own options.
        if (argument == L"--launch" || argument.starts_with(L"--launch="))
            break;
        if (argument == L"--config")
            return {ConfigurationArgumentStatus::Invalid, {}};
        if (!argument.starts_with(prefix))
            continue;
        if (result.status != ConfigurationArgumentStatus::Missing || argument.size() == prefix.size() ||
            HasEmbeddedNull(argument))
            return {ConfigurationArgumentStatus::Invalid, {}};
        result.status = ConfigurationArgumentStatus::Supplied;
        result.path = argument.substr(prefix.size());
    }
    return result;
}

ConfigurationArgument ReadProcessConfigurationArgument() {
    int count = 0;
    wchar_t** parsed = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!parsed)
        return {ConfigurationArgumentStatus::Invalid, {}};
    struct Arguments {
        wchar_t** value;
        ~Arguments() {
            LocalFree(static_cast<void*>(value));
        }
    } owned{parsed};
    std::vector<std::wstring> arguments;
    arguments.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index)
        arguments.emplace_back(parsed[index]);
    return ReadConfigurationArgument(arguments);
}

std::optional<RuntimePackagePaths> RuntimePackagePaths::FromExecutable(const std::wstring& executable,
                                                                       const ConfigurationArgument& configuration,
                                                                       PackagePathError& error) {
    error = PackagePathError::InvalidExecutable;
    if (executable.empty() || HasEmbeddedNull(executable) || !std::filesystem::path(executable).is_absolute())
        return std::nullopt;
    error = PackagePathError::InvalidConfiguration;
    if (configuration.status == ConfigurationArgumentStatus::Invalid)
        return std::nullopt;
    if (configuration.status != ConfigurationArgumentStatus::Missing &&
        configuration.status != ConfigurationArgumentStatus::Supplied)
        return std::nullopt;
    RuntimePackagePaths result;
    result.executable_ = std::filesystem::path(executable).lexically_normal().wstring();
    const auto directory = std::filesystem::path(result.executable_).parent_path();
    bool directoryExact = false;
    result.directory_ = ce::ansi_path::CompatiblePath(directory.wstring(), &directoryExact);
    if (configuration.status == ConfigurationArgumentStatus::Supplied) {
        const auto absolute = AbsoluteConfiguration(configuration.path);
        if (!absolute)
            return std::nullopt;
        result.configurationWide_ = *absolute;
        bool configurationExact = false;
        result.configuration_ = ce::ansi_path::CompatiblePath(*absolute, &configurationExact);
        if (!configurationExact) {
            const auto file = std::filesystem::path(*absolute);
            bool parentExact = false;
            const auto parent = ce::ansi_path::CompatiblePath(file.parent_path().wstring(), &parentExact);
            std::string leaf;
            if (parentExact && ce::ansi_path::TryNarrowAcpExactly(file.filename().wstring(), &leaf)) {
                result.configuration_ = parent + "\\" + leaf;
                configurationExact = true;
            }
        }
        result.encodingExact_ = directoryExact && configurationExact;
    } else {
        result.configurationWide_ = (directory / L"config.ini").wstring();
        // The INI can be absent on first launch. Convert the existing directory,
        // whose short name is available even before the default file is created.
        result.configuration_ = result.directory_ + "\\config.ini";
        result.encodingExact_ = directoryExact;
    }
    error = PackagePathError::None;
    return result;
}

std::optional<RuntimePackagePaths> RuntimePackagePaths::FromModule(HMODULE module,
                                                                   const ConfigurationArgument& configuration,
                                                                   PackagePathError& error) {
    std::wstring image = ce::ansi_path::ModulePathW(module);
    if (image.empty()) {
        error = PackagePathError::ModuleUnavailable;
        return std::nullopt;
    }
    if (module && module != GetModuleHandleW(nullptr))
        image = (std::filesystem::path(image).parent_path() / L"captureengine.exe").wstring();
    return FromExecutable(image, configuration, error);
}

std::wstring ActiveCodePagePathToWide(std::string_view path) {
    if (path.empty() || path.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        path.find('\0') != std::string_view::npos)
        return {};
    const int length = MultiByteToWideChar(CP_ACP, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, path.data(), static_cast<int>(path.size()), wide.data(), length) != length)
        return {};
    return wide;
}
}  // namespace ce::runtime
