#pragma once

// Decisions the installer makes that need no Windows API: command line, install
// directory rules, option defaults, the config.ini policy, the file manifest and
// what an uninstall may remove. Kept header-only so tests/test_installer_policy.cpp
// pins every rule without touching the machine.

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "payload_format.h"

namespace ce::setup {

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

enum Option : uint32_t {
    kOptDesktopShortcut = 1u << 0,
    kOptStartMenuShortcut = 1u << 1,
    kOptService = 1u << 2,
    kOptAutostart = 1u << 3,
    kOptElevated = 1u << 4,
    kOptPawnIo = 1u << 5,
    kOptLaunch = 1u << 6,
    kOptRemoveData = 1u << 7,  // uninstall only: also delete config.ini and logs
};

// Offered on a first installation. Autostart, the elevation service and the
// driver are what make a resident capture tool useful out of the box; every one
// of them can be unchecked on the options page.
inline constexpr uint32_t kFreshDefaults =
    kOptDesktopShortcut | kOptStartMenuShortcut | kOptService | kOptAutostart | kOptPawnIo | kOptLaunch;

struct ExistingState {
    bool installed = false;
    bool hasPreferences = false;
    bool service = false;
    bool autostart = false;
    bool elevated = false;
    bool desktopShortcut = false;
    bool startMenuShortcut = false;
    bool pawnIoInstalled = false;
};

// An update never silently flips a choice the user made earlier: every stored
// preference is carried over, and only unknown state falls back to the defaults.
inline uint32_t DefaultOptions(const ExistingState& existing) {
    if (!existing.installed)
        return kFreshDefaults & ~(existing.pawnIoInstalled ? static_cast<uint32_t>(kOptPawnIo) : 0u);
    uint32_t options = kOptLaunch;
    if (existing.desktopShortcut)
        options |= kOptDesktopShortcut;
    if (existing.startMenuShortcut)
        options |= kOptStartMenuShortcut;
    if (existing.hasPreferences) {
        if (existing.service)
            options |= kOptService;
        if (existing.autostart)
            options |= kOptAutostart;
        if (existing.elevated)
            options |= kOptElevated;
    } else {
        options |= kOptService | kOptAutostart;
    }
    if (!existing.pawnIoInstalled)
        options |= kOptPawnIo;
    return options;
}

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

enum class Mode { Install, Uninstall, Help, Extract, Preview, VerifyPayload };

struct CommandLine {
    Mode mode = Mode::Install;
    bool silent = false;
    bool valid = true;
    std::string error;
    std::wstring directory;
    std::wstring extractDirectory;
    std::wstring previewDirectory;
    std::wstring waitProcess;
    uint32_t optionsOn = 0;
    uint32_t optionsOff = 0;
    bool removeUserData = false;
    bool fromTemporaryCopy = false;
    // Testing: copy or remove the files only; no registry, service, shortcuts or driver.
    bool filesOnly = false;
    // How long a running Capture Engine gets to quit before it is terminated.
    unsigned closeTimeoutSeconds = 30;
};

inline bool EqualsNoCase(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size())
        return false;
    for (size_t index = 0; index < left.size(); ++index) {
        wchar_t a = left[index];
        wchar_t b = right[index];
        if (a >= L'A' && a <= L'Z')
            a = static_cast<wchar_t>(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z')
            b = static_cast<wchar_t>(b - L'A' + L'a');
        if (a != b)
            return false;
    }
    return true;
}

inline bool StartsWithNoCase(std::wstring_view text, std::wstring_view prefix) {
    return text.size() >= prefix.size() && EqualsNoCase(text.substr(0, prefix.size()), prefix);
}

// `arguments` excludes argv[0]. Accepts the conventional installer spellings
// (/S, /D=path) next to the long forms so unattended deployments need no manual.
inline CommandLine ParseCommandLine(const std::vector<std::wstring>& arguments) {
    struct Toggle {
        const wchar_t* name;
        Option option;
    };
    static constexpr Toggle kToggles[] = {
        {L"desktop", kOptDesktopShortcut}, {L"start-menu", kOptStartMenuShortcut}, {L"service", kOptService},
        {L"autostart", kOptAutostart},     {L"admin", kOptElevated},               {L"pawnio", kOptPawnIo},
        {L"launch", kOptLaunch},
    };
    CommandLine result;
    const auto fail = [&](const std::string& message) {
        if (result.valid) {
            result.valid = false;
            result.error = message;
        }
    };
    for (const std::wstring& argument : arguments) {
        std::wstring_view text(argument);
        if (EqualsNoCase(text, L"/S") || EqualsNoCase(text, L"--silent") || EqualsNoCase(text, L"/silent")) {
            result.silent = true;
        } else if (EqualsNoCase(text, L"--uninstall") || EqualsNoCase(text, L"/uninstall")) {
            result.mode = Mode::Uninstall;
        } else if (EqualsNoCase(text, L"/?") || EqualsNoCase(text, L"--help") || EqualsNoCase(text, L"-h")) {
            result.mode = Mode::Help;
        } else if (EqualsNoCase(text, L"--verify-payload")) {
            result.mode = Mode::VerifyPayload;
        } else if (EqualsNoCase(text, L"--remove-data")) {
            result.removeUserData = true;
        } else if (EqualsNoCase(text, L"--keep-data")) {
            result.removeUserData = false;
        } else if (EqualsNoCase(text, L"--from-temporary-copy")) {
            result.fromTemporaryCopy = true;
        } else if (EqualsNoCase(text, L"--files-only")) {
            result.filesOnly = true;
        } else if (StartsWithNoCase(text, L"/D=")) {
            result.directory.assign(text.substr(3));
        } else if (StartsWithNoCase(text, L"--dir=")) {
            result.directory.assign(text.substr(6));
        } else if (StartsWithNoCase(text, L"--extract=")) {
            result.mode = Mode::Extract;
            result.extractDirectory.assign(text.substr(10));
        } else if (StartsWithNoCase(text, L"--preview=")) {
            result.mode = Mode::Preview;
            result.previewDirectory.assign(text.substr(10));
        } else if (StartsWithNoCase(text, L"--wait-process=")) {
            result.waitProcess.assign(text.substr(15));
        } else if (StartsWithNoCase(text, L"--close-timeout=")) {
            const std::wstring_view number = text.substr(16);
            unsigned value = 0;
            bool digits = !number.empty() && number.size() <= 3;
            for (wchar_t character : number) {
                digits = digits && character >= L'0' && character <= L'9';
                value = value * 10 + static_cast<unsigned>(character - L'0');
            }
            if (!digits || value < 1 || value > 300)
                fail("--close-timeout needs a number of seconds from 1 to 300.");
            else
                result.closeTimeoutSeconds = value;
        } else {
            bool matched = false;
            for (const Toggle& toggle : kToggles) {
                const std::wstring positive = std::wstring(L"--") + toggle.name;
                const std::wstring negative = std::wstring(L"--no-") + toggle.name;
                if (EqualsNoCase(text, positive)) {
                    result.optionsOn |= toggle.option;
                    result.optionsOff &= ~static_cast<uint32_t>(toggle.option);
                    matched = true;
                } else if (EqualsNoCase(text, negative)) {
                    result.optionsOff |= toggle.option;
                    result.optionsOn &= ~static_cast<uint32_t>(toggle.option);
                    matched = true;
                }
                if (matched)
                    break;
            }
            if (!matched)
                fail("Unknown option: " + std::string(text.begin(), text.end()));
        }
    }
    if (result.mode == Mode::Extract && result.extractDirectory.empty())
        fail("--extract needs a directory.");
    if (result.mode == Mode::Preview && result.previewDirectory.empty())
        fail("--preview needs a directory.");
    return result;
}

inline uint32_t ApplyOverrides(uint32_t defaults, const CommandLine& command) {
    return (defaults | command.optionsOn) & ~command.optionsOff;
}

// ---------------------------------------------------------------------------
// Install directory
// ---------------------------------------------------------------------------

enum class DirectoryIssue {
    Ok,
    Empty,
    NotAbsolute,
    Unc,
    DevicePath,
    DriveRoot,
    TooLong,
    InvalidCharacter,
    InvalidComponent,
    InsideWindows,
};

inline std::wstring NormalizeDirectory(std::wstring_view input) {
    size_t begin = 0;
    size_t end = input.size();
    while (begin < end && (input[begin] == L' ' || input[begin] == L'\t'))
        ++begin;
    while (end > begin && (input[end - 1] == L' ' || input[end - 1] == L'\t'))
        --end;
    if (end - begin >= 2 && input[begin] == L'"' && input[end - 1] == L'"') {
        ++begin;
        --end;
    }
    std::wstring result(input.substr(begin, end - begin));
    for (wchar_t& character : result) {
        if (character == L'/')
            character = L'\\';
    }
    // Collapse repeated separators after the drive, keeping no trailing one.
    std::wstring collapsed;
    collapsed.reserve(result.size());
    for (size_t index = 0; index < result.size(); ++index) {
        if (result[index] == L'\\' && index > 0 && result[index - 1] == L'\\' && index > 1)
            continue;
        collapsed.push_back(result[index]);
    }
    while (collapsed.size() > 3 && collapsed.back() == L'\\')
        collapsed.pop_back();
    return collapsed;
}

inline bool IsPathInside(std::wstring_view path, std::wstring_view root) {
    std::wstring left = NormalizeDirectory(path);
    std::wstring right = NormalizeDirectory(root);
    if (right.empty() || left.size() < right.size() || !EqualsNoCase(std::wstring_view(left).substr(0, right.size()), right))
        return false;
    if (left.size() == right.size())
        return true;
    return right.back() == L'\\' || left[right.size()] == L'\\';
}

inline DirectoryIssue ValidateInstallDirectory(std::wstring_view directory, std::wstring_view windowsDirectory) {
    const std::wstring path = NormalizeDirectory(directory);
    if (path.empty())
        return DirectoryIssue::Empty;
    if (path.rfind(L"\\\\?\\", 0) == 0 || path.rfind(L"\\\\.\\", 0) == 0)
        return DirectoryIssue::DevicePath;
    if (path.rfind(L"\\\\", 0) == 0)
        return DirectoryIssue::Unc;
    const bool drive = path.size() >= 2 && path[1] == L':' &&
                       ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'));
    if (!drive || path.size() < 3 || path[2] != L'\\')
        return path.size() == 2 && drive ? DirectoryIssue::DriveRoot : DirectoryIssue::NotAbsolute;
    if (path.size() == 3)
        return DirectoryIssue::DriveRoot;
    // Leave headroom under MAX_PATH for the longest payload path and ".cenew".
    if (path.size() > 200)
        return DirectoryIssue::TooLong;
    for (size_t index = 3; index < path.size(); ++index) {
        const wchar_t character = path[index];
        if (character < 0x20 || character == L'<' || character == L'>' || character == L':' || character == L'"' ||
            character == L'|' || character == L'?' || character == L'*')
            return DirectoryIssue::InvalidCharacter;
    }
    size_t start = 3;
    while (start <= path.size()) {
        size_t end = path.find(L'\\', start);
        if (end == std::wstring::npos)
            end = path.size();
        const std::wstring_view component = std::wstring_view(path).substr(start, end - start);
        if (component.empty() || component == L"." || component == L".." || component.back() == L'.' ||
            component.back() == L' ')
            return DirectoryIssue::InvalidComponent;
        if (end == path.size())
            break;
        start = end + 1;
    }
    if (!windowsDirectory.empty() && IsPathInside(path, windowsDirectory))
        return DirectoryIssue::InsideWindows;
    return DirectoryIssue::Ok;
}

inline const char* DirectoryIssueText(DirectoryIssue issue) {
    switch (issue) {
    case DirectoryIssue::Ok:
        return "";
    case DirectoryIssue::Empty:
        return "Enter the folder Capture Engine should be installed in.";
    case DirectoryIssue::NotAbsolute:
        return "Enter a full folder path that starts with a drive letter, for example C:\\Program Files\\Capture Engine.";
    case DirectoryIssue::Unc:
        return "Network folders are not supported. Choose a folder on a local drive.";
    case DirectoryIssue::DevicePath:
        return "That path form is not supported. Enter an ordinary folder path.";
    case DirectoryIssue::DriveRoot:
        return "Choose a folder, not a drive root. Capture Engine needs a folder of its own.";
    case DirectoryIssue::TooLong:
        return "That folder path is too long. Choose a shorter one.";
    case DirectoryIssue::InvalidCharacter:
        return "The folder path contains a character Windows does not allow in names.";
    case DirectoryIssue::InvalidComponent:
        return "A folder name in the path is not valid (empty, ending in a dot or space, or . / ..).";
    case DirectoryIssue::InsideWindows:
        return "Do not install inside the Windows folder.";
    }
    return "";
}

// ---------------------------------------------------------------------------
// config.ini
// ---------------------------------------------------------------------------

inline constexpr char kConfigName[] = "config.ini";
inline constexpr char kConfigTemplateName[] = "config.ini.new";

struct ConfigPlan {
    bool writeConfig = false;
    bool writeTemplate = false;
};

// The user's file is never overwritten or merged. A missing one is created; an
// existing one stays exactly as it is and the shipped defaults are written next
// to it so new options can be compared and copied over by hand.
inline ConfigPlan PlanConfig(bool configExists) {
    return {!configExists, configExists};
}

// ---------------------------------------------------------------------------
// User data: never installed over, never removed by an update
// ---------------------------------------------------------------------------

inline constexpr const char* kDataDirectories[] = {"logs", "captures", "screenshots", "benchmarks"};

inline bool IsUserDataPath(std::string_view relative) {
    const std::string lower = AsciiLowerCopy(relative);
    if (lower == "config.ini")
        return true;
    for (const char* directory : kDataDirectories) {
        const std::string name(directory);
        if (lower == name || lower.compare(0, name.size() + 1, name + "/") == 0)
            return true;
    }
    return false;
}

// Files the installer itself wrote and may delete again without asking.
inline bool IsInstallerOwnedLeaf(std::string_view relative) {
    const std::string lower = AsciiLowerCopy(relative);
    return lower == "config.ini.new" || lower == "captureengine_install.manifest";
}

inline constexpr char kStagingSuffix[] = ".cenew";
inline constexpr char kBackupSuffix[] = ".cebak";

inline bool IsInstallerTemporaryName(std::string_view name) {
    const std::string lower = AsciiLowerCopy(name);
    const auto ends = [&](const char* suffix) {
        const size_t length = std::char_traits<char>::length(suffix);
        return lower.size() > length && lower.compare(lower.size() - length, length, suffix) == 0;
    };
    return ends(kStagingSuffix) || ends(kBackupSuffix);
}

// ---------------------------------------------------------------------------
// Manifest: the exact set of files an installation wrote
// ---------------------------------------------------------------------------

inline constexpr char kManifestHeader[] = "CEINSTALL1";

inline std::string SerializeManifest(std::string_view version, const std::vector<std::string>& files) {
    std::string text = std::string(kManifestHeader) + "\nversion=" + std::string(version) + "\n";
    for (const std::string& file : files)
        text += "file=" + file + "\n";
    return text;
}

struct Manifest {
    bool valid = false;
    std::string version;
    std::vector<std::string> files;
};

inline Manifest ParseManifest(std::string_view text) {
    Manifest manifest;
    size_t start = 0;
    bool first = true;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        start = end + 1;
        if (first) {
            if (line != kManifestHeader)
                return {};
            first = false;
            continue;
        }
        if (line.empty())
            continue;
        if (line.rfind("version=", 0) == 0)
            manifest.version.assign(line.substr(8));
        else if (line.rfind("file=", 0) == 0) {
            const std::string path(line.substr(5));
            // A manifest is data on disk; whatever it names is validated again so
            // a damaged or edited one can never direct a deletion outside the folder.
            if (IsSafeRelativePath(path))
                manifest.files.push_back(path);
        }
    }
    manifest.valid = !first;
    return manifest;
}

// Files the previous installation wrote that the new payload no longer ships.
inline std::vector<std::string> StaleFiles(const Manifest& previous, const std::vector<std::string>& current) {
    std::set<std::string> keep;
    for (const std::string& file : current)
        keep.insert(AsciiLowerCopy(file));
    std::vector<std::string> stale;
    for (const std::string& file : previous.files) {
        if (!keep.count(AsciiLowerCopy(file)) && !IsUserDataPath(file))
            stale.push_back(file);
    }
    return stale;
}

// Directories (relative, deepest first) that hold the manifest's files, so an
// uninstall can remove them again when - and only when - they end up empty.
inline std::vector<std::string> ParentDirectories(const std::vector<std::string>& files) {
    std::set<std::string> directories;
    for (const std::string& file : files) {
        for (size_t slash = file.find('/'); slash != std::string::npos; slash = file.find('/', slash + 1))
            directories.insert(file.substr(0, slash));
    }
    std::vector<std::string> ordered(directories.begin(), directories.end());
    std::sort(ordered.begin(), ordered.end(), [](const std::string& left, const std::string& right) {
        const auto depth = [](const std::string& value) { return std::count(value.begin(), value.end(), '/'); };
        return depth(left) != depth(right) ? depth(left) > depth(right) : left > right;
    });
    return ordered;
}

// A shortcut belongs to Capture Engine when it points at the installed exe; a
// same-named link to anything else is somebody else's and is left alone.
inline bool ShortcutTargetsInstallation(std::wstring_view target, std::wstring_view installDirectory,
                                        std::wstring_view exeName) {
    const std::wstring normalized = NormalizeDirectory(target);
    const size_t slash = normalized.find_last_of(L'\\');
    if (slash == std::wstring::npos)
        return false;
    return EqualsNoCase(std::wstring_view(normalized).substr(slash + 1), exeName) &&
           EqualsNoCase(NormalizeDirectory(std::wstring_view(normalized).substr(0, slash)),
                        NormalizeDirectory(installDirectory));
}

}  // namespace ce::setup
