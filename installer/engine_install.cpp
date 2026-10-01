// Installing or updating Capture Engine.
//
// The file set changes as one unit: every payload file is first extracted
// beside its destination (".cenew"), then the existing file is renamed aside
// (".cebak") and the new one renamed into place. A failure at any point puts
// the previous files back, so an update never leaves a half-replaced program.
// Registration (shortcuts, startup, service, driver) happens only after the
// files are committed and reports problems as warnings: the program is already
// usable by then.
//
// config.ini is never part of that unit. It is created when missing; when it
// exists it is left byte-for-byte alone and the shipped defaults are written to
// config.ini.new beside it.

#include "setup.h"

#include <algorithm>

namespace ce::setup {
namespace {

constexpr uint64_t kDiskSlackBytes = 64ull << 20;

struct StagedFile {
    const FileEntry* entry = nullptr;
    std::wstring destination;
    std::wstring staging;
    std::wstring backup;
    bool hadOriginal = false;
    bool swapped = false;
};

// Undoes a failed file commit. Registration is not part of it.
class Rollback {
public:
    explicit Rollback(std::vector<StagedFile>& files) : files_(files) {}
    void NoteCreatedDirectory(const std::wstring& path) { directories_.push_back(path); }
    void Run() {
        for (auto it = files_.rbegin(); it != files_.rend(); ++it) {
            if (it->swapped) {
                DeleteFileW(it->destination.c_str());
                if (it->hadOriginal && !MoveFileExW(it->backup.c_str(), it->destination.c_str(), MOVEFILE_WRITE_THROUGH))
                    Log("rollback: could not restore %s (error %lu)", Narrow(it->destination).c_str(), GetLastError());
            }
            DeleteFileW(it->staging.c_str());
        }
        // Only the folders this run created, and only while empty.
        for (auto it = directories_.rbegin(); it != directories_.rend(); ++it)
            RemoveDirectoryW(it->c_str());
    }

private:
    std::vector<StagedFile>& files_;
    std::vector<std::wstring> directories_;
};

std::wstring FormatMegabytes(uint64_t bytes) {
    return std::to_wstring((bytes + (1u << 20) - 1) >> 20) + L" MB";
}

bool CreateDirectoryTracked(const std::wstring& path, Rollback& rollback, DWORD* error) {
    if (DirectoryExists(path))
        return true;
    const std::wstring parent = DirectoryOf(path);
    if (!parent.empty() && parent != path && !CreateDirectoryTracked(parent, rollback, error))
        return false;
    if (CreateDirectoryW(path.c_str(), nullptr)) {
        rollback.NoteCreatedDirectory(path);
        return true;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return true;
    if (error)
        *error = GetLastError();
    return false;
}

void Fail(InstallResult& result, const std::wstring& message) {
    result.success = false;
    result.error = message;
    Log("install failed: %s", Narrow(message).c_str());
}

// Everything past the point of no return: registration reports warnings only.
void RegisterInstallation(const InstallRequest& request, const std::wstring& directory, uint64_t totalBytes,
                          InstallResult& result, const ProgressFn& progress) {
    const auto warn = [&](const std::wstring& message) {
        result.warnings.push_back(message);
        Log("warning: %s", Narrow(message).c_str());
    };
    if (progress)
        progress(86, L"Registering Capture Engine...");
    DWORD error = 0;
    if (!WriteUninstallRecord(directory, totalBytes, &error))
        warn(L"Capture Engine could not be added to Installed Apps: " + ErrorText(error));

    const auto shortcut = [&](uint32_t option, const std::wstring& link, const wchar_t* label) {
        if (request.options & option) {
            if (!CreateAppShortcut(link, directory, &error))
                warn(std::wstring(L"The ") + label + L" shortcut could not be created: " + ErrorText(error));
        } else if (!RemoveOwnedShortcut(link, directory)) {
            warn(std::wstring(L"The old ") + label + L" shortcut could not be removed.");
        }
    };
    shortcut(kOptStartMenuShortcut, StartMenuShortcutPath(), L"Start menu");
    shortcut(kOptDesktopShortcut, DesktopShortcutPath(), L"desktop");

    if (progress)
        progress(90, L"Configuring startup and the elevation service...");
    std::wstring sid;
    bool administrator = false;
    IntegrationRequest role;
    role.directory = directory;
    role.options = request.options;
    role.applyAutostart = true;
    role.applyService = (request.options & kOptService) != 0;
    role.removeService = !role.applyService && ElevationServiceExists();
    if (!ResolveOwner(&sid, &administrator) || sid == L"S-1-5-18") {
        // LocalSystem is what a deployment tool runs as; startup entries and the
        // service's allowed user belong to a person, not to that account.
        warn(L"No signed-in user could be determined, so startup and the elevation service were not configured. "
             L"Toggle them from the Capture Engine tray menu.");
    } else {
        role.ownerSid = sid;
        role.ownerAdministrator = administrator;
        const DWORD roleResult = RunIntegrationRole(role);
        if (roleResult != ERROR_SUCCESS) {
            warn(L"Startup or the elevation service could not be configured: " + ErrorText(roleResult) +
                 L" You can toggle both from the Capture Engine tray menu.");
        } else {
            result.serviceEnabled = role.applyService;
            result.startupConfigured = true;
        }
    }

    if ((request.options & kOptPawnIo) && !IsPawnIoInstalled()) {
        if (progress)
            progress(95, L"Installing the PawnIO driver...");
        const DWORD driver = RunPawnIoRole(directory);
        if (driver == ERROR_SUCCESS)
            result.pawnIoInstalled = true;
        else
            warn(L"The PawnIO driver could not be installed (code " + std::to_wstring(driver) +
                 L"). CPU temperature and power readings stay unavailable until it is installed.");
    }
}

}  // namespace

InstallResult RunInstall(PayloadReader& payload, const InstallRequest& request, const ProgressFn& progress) {
    InstallResult result;
    const auto report = [&](int percent, const std::wstring& status) {
        Log("[%d%%] %s", percent, Narrow(status).c_str());
        if (progress)
            progress(percent, status);
    };
    const std::wstring directory = NormalizeDirectory(request.directory);
    Log("install: %s version %s, options 0x%x", Narrow(directory).c_str(), kVersionUtf8, request.options);

    // 1. Preflight ----------------------------------------------------------
    report(1, L"Checking the installation folder...");
    const DirectoryIssue issue = ValidateInstallDirectory(directory, WindowsDirectory());
    if (issue != DirectoryIssue::Ok) {
        Fail(result, Widen(DirectoryIssueText(issue)));
        return result;
    }
    if (PathExists(directory) && !DirectoryExists(directory)) {
        Fail(result, L"A file with that name already exists. Choose a different folder.");
        return result;
    }
    if (DirectoryExists(directory) && IsReparsePoint(directory))
        Log("install: the target folder is itself a link; continuing");
    ULARGE_INTEGER freeBytes{};
    const std::wstring anchor = NearestExistingDirectory(directory);
    if (!anchor.empty() && GetDiskFreeSpaceExW(anchor.c_str(), &freeBytes, nullptr, nullptr)) {
        const uint64_t needed = payload.TotalSize() + kDiskSlackBytes;
        if (freeBytes.QuadPart < needed) {
            Fail(result, L"There is not enough free disk space. " + FormatMegabytes(needed) + L" are needed, " +
                             FormatMegabytes(freeBytes.QuadPart) + L" are available.");
            return result;
        }
    }

    // The old install, wherever it is: a move leaves nothing running or behind.
    const ExistingInstall existing = request.filesOnly ? ExistingInstall() : ReadExistingInstall(directory);
    const std::wstring previousDirectory =
        existing.registered && !EqualsNoCase(NormalizeDirectory(existing.directory), directory)
            ? NormalizeDirectory(existing.directory)
            : std::wstring();

    // 2. Close what runs from the folder -------------------------------------
    report(3, L"Closing Capture Engine...");
    std::wstring closeError;
    if (!CloseRunningInstances(directory, &closeError, progress, !request.filesOnly, request.closeTimeoutSeconds)) {
        Fail(result, closeError);
        return result;
    }
    if (!previousDirectory.empty() &&
        !CloseRunningInstances(previousDirectory, &closeError, progress, true, request.closeTimeoutSeconds)) {
        Fail(result, closeError);
        return result;
    }

    // 3. Stage -------------------------------------------------------------
    std::vector<StagedFile> files;
    Rollback rollback(files);
    DWORD error = 0;
    if (!CreateDirectoryTracked(directory, rollback, &error)) {
        Fail(result, L"The installation folder could not be created: " + ErrorText(error));
        return result;
    }
    Manifest previous;
    LoadManifest(directory, &previous);

    std::vector<std::string> shipped;
    uint64_t totalBytes = 0;
    for (const FileEntry& entry : payload.Files()) {
        // The user's configuration is handled separately and never staged.
        if (EqualsNoCase(Widen(entry.path), L"config.ini"))
            continue;
        shipped.push_back(entry.path);
        totalBytes += entry.size;
        StagedFile staged;
        staged.entry = &entry;
        staged.destination = JoinPath(directory, Widen(entry.path));
        staged.staging = staged.destination + kStagingSuffixWide;
        staged.backup = staged.destination + kBackupSuffixWide;
        files.push_back(std::move(staged));
    }
    CleanTemporaryFiles(directory, shipped);

    uint64_t extracted = 0;
    for (StagedFile& staged : files) {
        if (HasReparseAncestor(directory, staged.entry->path)) {
            rollback.Run();
            Fail(result, L"A folder inside the installation is a link and cannot be installed into: " +
                             Widen(staged.entry->path));
            return result;
        }
        const std::wstring parent = DirectoryOf(staged.destination);
        if (!CreateDirectoryTracked(parent, rollback, &error)) {
            rollback.Run();
            Fail(result, L"A folder could not be created: " + ErrorText(error));
            return result;
        }
        if (DirectoryExists(staged.destination)) {
            rollback.Run();
            Fail(result, L"A folder is in the way of " + Widen(staged.entry->path) + L".");
            return result;
        }
        DeleteFileW(staged.staging.c_str());
        const uint64_t before = extracted;
        const PayloadStatus status = payload.ExtractFile(*staged.entry, staged.staging, &error, [&](uint64_t done) {
            const uint64_t absolute = before + done;
            const int percent = 5 + static_cast<int>(65 * absolute / std::max<uint64_t>(totalBytes, 1));
            if (progress)
                progress(percent, L"Copying " + Widen(staged.entry->path));
        });
        if (status != PayloadStatus::Ok) {
            rollback.Run();
            std::wstring message = L"Could not write " + Widen(staged.entry->path) + L": " +
                                   Widen(PayloadStatusText(status));
            if (status == PayloadStatus::IoError)
                message += L" " + ErrorText(error);
            Fail(result, message);
            return result;
        }
        extracted += staged.entry->size;
    }

    // 4. Commit --------------------------------------------------------------
    report(72, L"Replacing program files...");
    for (StagedFile& staged : files) {
        staged.hadOriginal = PathExists(staged.destination);
        if (staged.hadOriginal) {
            DeleteFileW(staged.backup.c_str());
            if (!MoveFileExW(staged.destination.c_str(), staged.backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
                const DWORD moveError = GetLastError();
                rollback.Run();
                Fail(result, Widen(staged.entry->path) + L" is in use and cannot be replaced: " + ErrorText(moveError));
                return result;
            }
        }
        // Counted as swapped from here on: even a failed rename must restore the backup.
        staged.swapped = true;
        if (!MoveFileExW(staged.staging.c_str(), staged.destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            const DWORD moveError = GetLastError();
            rollback.Run();
            Fail(result, L"Could not install " + Widen(staged.entry->path) + L": " + ErrorText(moveError));
            return result;
        }
    }

    // 5. Configuration -----------------------------------------------------
    report(80, L"Preparing your configuration...");
    if (const FileEntry* config = payload.Find("config.ini")) {
        std::string contents;
        if (payload.ReadContents(*config, &contents, &error) == PayloadStatus::Ok) {
            const std::wstring configPath = JoinPath(directory, L"config.ini");
            const ConfigPlan plan = PlanConfig(PathExists(configPath));
            if (plan.writeConfig) {
                result.configWritten = WriteFileAtomically(configPath, contents.data(), contents.size(), false, &error);
                if (result.configWritten)
                    GrantUsersModify(configPath, false, &error);
                else
                    result.warnings.push_back(L"The default config.ini could not be written: " + ErrorText(error));
            }
            if (plan.writeTemplate) {
                const std::wstring templatePath = JoinPath(directory, L"config.ini.new");
                result.templateWritten =
                    WriteFileAtomically(templatePath, contents.data(), contents.size(), true, &error);
                if (result.templateWritten)
                    GrantUsersModify(templatePath, false, &error);
                else
                    result.warnings.push_back(L"config.ini.new could not be written: " + ErrorText(error));
            }
            Log("config: created=%d template=%d", result.configWritten ? 1 : 0, result.templateWritten ? 1 : 0);
        } else {
            result.warnings.push_back(L"The default configuration could not be read from the setup file.");
        }
    }

    // 6. Folders the program writes at run time ------------------------------
    report(83, L"Setting folder permissions...");
    for (const char* name : kDataDirectories) {
        const std::wstring folder = JoinPath(directory, Widen(name));
        if (!CreateDirectoryTree(folder, &error) || !GrantUsersModify(folder, true, &error))
            result.warnings.push_back(L"The " + Widen(name) + L" folder could not be prepared: " + ErrorText(error));
    }

    // 7. Manifest, uninstaller bookkeeping, leftovers ------------------------
    report(85, L"Finishing up...");
    for (const StagedFile& staged : files) {
        if (!staged.hadOriginal)
            continue;
        bool deferred = false;
        if (!RemoveOrRenameAway(staged.backup, &deferred))
            Log("cleanup: previous %s kept as %s", Narrow(staged.destination).c_str(), Narrow(staged.backup).c_str());
    }
    for (const std::string& stale : StaleFiles(previous, shipped)) {
        if (HasReparseAncestor(directory, stale))
            continue;
        bool deferred = false;
        if (!RemoveOrRenameAway(JoinPath(directory, Widen(stale)), &deferred))
            result.warnings.push_back(L"The previous version's file " + Widen(stale) + L" could not be removed.");
    }
    std::vector<std::string> manifestFiles = shipped;
    std::sort(manifestFiles.begin(), manifestFiles.end());
    const std::string manifest = SerializeManifest(kVersionUtf8, manifestFiles);
    if (!WriteFileAtomically(JoinPath(directory, kManifestFile), manifest.data(), manifest.size(), true, &error))
        result.warnings.push_back(L"The file list could not be written, so Uninstall cannot remove every file: " +
                                  ErrorText(error));

    // 8. Registration (warnings only) ----------------------------------------
    if (!request.filesOnly)
        RegisterInstallation(request, directory, totalBytes, result, progress);

    // A moved installation: the old folder's files go, its data stays.
    if (!previousDirectory.empty()) {
        Manifest old;
        if (LoadManifest(previousDirectory, &old)) {
            report(98, L"Removing the previous installation folder...");
            RemoveManifestFiles(previousDirectory, old.files, &result.warnings, &result.rebootRecommended);
            RemoveEmptyDirectories(previousDirectory, old.files);
            RemoveOrRenameAway(JoinPath(previousDirectory, kManifestFile), nullptr);
            RemoveDirectoryW(previousDirectory.c_str());
        } else {
            result.warnings.push_back(L"The previous installation in " + previousDirectory +
                                      L" was left in place. You can delete that folder yourself.");
        }
    }

    report(100, L"Done.");
    result.success = true;
    return result;
}

}  // namespace ce::setup
