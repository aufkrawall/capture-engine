// Removing Capture Engine.
//
// Order: close the program, let the program's own role remove the service and
// startup entries (it knows exactly what it registered), then delete the files
// the installation manifest lists. Recordings, screenshots and benchmark
// reports are never touched; configuration and logs only when asked.

#include "setup.h"

#include <set>

namespace ce::setup {
namespace {

// Deletes a folder tree without following links: a junction or symlink inside
// is removed as a link, never traversed.
void RemoveTreeNoFollow(const std::wstring& path, int depth = 0) {
    if (depth > 32)
        return;
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(JoinPath(path, L"*").c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..")
                continue;
            const std::wstring child = JoinPath(path, name);
            const bool link = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !link) {
                RemoveTreeNoFollow(child, depth + 1);
            } else {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                    SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (link && (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    RemoveDirectoryW(child.c_str());
                else
                    DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    RemoveDirectoryW(path.c_str());
}

void DeleteUserRegistry(const std::wstring& sid) {
    if (sid.empty())
        return;
    const std::wstring key = sid + L"\\Software\\CaptureEngine";
    const LSTATUS status = RegDeleteTreeW(HKEY_USERS, key.c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        Log("uninstall: could not delete the per-user settings key (error %ld)", status);
}

// Last resort when the installed program is gone: the Run value is all that
// can be removed without its help.
void DeleteRunValue(const std::wstring& sid) {
    if (sid.empty())
        return;
    const std::wstring key = sid + L"\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    HKEY handle = nullptr;
    if (RegOpenKeyExW(HKEY_USERS, key.c_str(), 0, KEY_SET_VALUE, &handle) == ERROR_SUCCESS) {
        RegDeleteValueW(handle, L"CaptureEngine");
        RegCloseKey(handle);
    }
}

}  // namespace

UninstallResult RunUninstall(const UninstallRequest& request, const ProgressFn& progress) {
    UninstallResult result;
    const auto report = [&](int percent, const std::wstring& status) {
        Log("[%d%%] %s", percent, Narrow(status).c_str());
        if (progress)
            progress(percent, status);
    };
    const auto warn = [&](const std::wstring& message) {
        result.warnings.push_back(message);
        Log("warning: %s", Narrow(message).c_str());
    };
    const std::wstring directory = NormalizeDirectory(request.directory);
    Log("uninstall: %s (remove data: %d)", Narrow(directory).c_str(), request.removeUserData ? 1 : 0);

    const DirectoryIssue issue = ValidateInstallDirectory(directory, WindowsDirectory());
    if (issue != DirectoryIssue::Ok) {
        result.error = Widen(DirectoryIssueText(issue));
        return result;
    }
    Manifest manifest;
    const bool haveManifest = LoadManifest(directory, &manifest);
    if (!haveManifest && !PathExists(JoinPath(directory, kAppExe))) {
        result.error = L"Capture Engine is not installed in " + directory + L".";
        return result;
    }

    report(5, L"Closing Capture Engine...");
    std::wstring closeError;
    if (!CloseRunningInstances(directory, &closeError, progress, !request.filesOnly, request.closeTimeoutSeconds)) {
        result.error = closeError;
        return result;
    }

    // The program removes its own service and startup registration.
    report(20, L"Removing the elevation service and startup entry...");
    std::wstring interactiveSid;
    bool interactiveAdministrator = false;
    if (!request.filesOnly)
        ResolveOwner(&interactiveSid, &interactiveAdministrator);
    const std::wstring serviceOwner = request.filesOnly ? std::wstring() : RegisteredServiceOwner();
    const bool exeAvailable = PathExists(JoinPath(directory, kAppExe));
    if (request.filesOnly) {
        // Testing mode: nothing outside the folder is read or changed.
    } else if (exeAvailable) {
        const bool serviceExists = ElevationServiceExists();
        if (serviceExists && !serviceOwner.empty()) {
            IntegrationRequest role;
            role.directory = directory;
            role.removeService = true;
            role.applyAutostart = true;
            role.ownerSid = serviceOwner;
            const DWORD code = RunIntegrationRole(role);
            if (code != ERROR_SUCCESS)
                Log("uninstall: service role for the registered owner returned %lu", static_cast<unsigned long>(code));
        }
        if (!interactiveSid.empty() && (!serviceExists || interactiveSid != serviceOwner)) {
            IntegrationRequest role;
            role.directory = directory;
            role.applyAutostart = true;
            role.ownerSid = interactiveSid;
            role.ownerAdministrator = interactiveAdministrator;
            const DWORD code = RunIntegrationRole(role);
            if (code != ERROR_SUCCESS)
                Log("uninstall: startup role for the signed-in user returned %lu", static_cast<unsigned long>(code));
        }
    } else {
        DeleteRunValue(interactiveSid);
        DeleteRunValue(serviceOwner);
    }
    // The role's exit code is advisory; the service manager has the truth.
    if (!request.filesOnly && ElevationServiceExists())
        warn(L"The Capture Engine elevation service could not be removed. Remove it from the tray menu of an "
             L"installed copy, or run 'sc delete CaptureEngineElevation' as administrator.");

    report(40, L"Removing shortcuts...");
    if (!request.filesOnly) {
        if (!RemoveOwnedShortcut(StartMenuShortcutPath(), directory))
            warn(L"The Start menu shortcut could not be removed.");
        if (!RemoveOwnedShortcut(DesktopShortcutPath(), directory))
            warn(L"The desktop shortcut could not be removed.");
    }

    report(50, L"Removing program files...");
    if (haveManifest) {
        RemoveManifestFiles(directory, manifest.files, &result.warnings, &result.rebootRecommended);
        RemoveEmptyDirectories(directory, manifest.files);
        CleanTemporaryFiles(directory, manifest.files);
    } else {
        warn(L"The list of installed files is missing, so the program files were left in place.");
    }
    bool ignored = false;
    RemoveOrRenameAway(JoinPath(directory, L"config.ini.new"), &ignored);

    if (request.removeUserData) {
        report(85, L"Removing settings and logs...");
        bool deferred = false;
        if (!RemoveOrRenameAway(JoinPath(directory, L"config.ini"), &deferred))
            warn(L"config.ini could not be removed.");
        RemoveTreeNoFollow(JoinPath(directory, L"logs"));
        if (!request.filesOnly) {
            DeleteUserRegistry(interactiveSid);
            if (serviceOwner != interactiveSid)
                DeleteUserRegistry(serviceOwner);
        }
    }
    if (haveManifest)
        RemoveOrRenameAway(JoinPath(directory, kManifestFile), &result.rebootRecommended);

    report(92, L"Removing the Installed Apps entry...");
    if (!request.filesOnly && !RemoveUninstallRecord())
        warn(L"The Installed Apps entry could not be removed.");

    // Empty folders only: recordings and anything else the user kept stay put.
    for (const char* name : kDataDirectories)
        RemoveDirectoryW(JoinPath(directory, Widen(name)).c_str());
    RemoveDirectoryW(directory.c_str());
    // The service's protected runtime parent, left behind once its child is gone.
    const std::wstring programFiles = request.filesOnly ? std::wstring() : ProgramFilesDirectory();
    if (!programFiles.empty())
        RemoveDirectoryW(JoinPath(programFiles, L"CaptureEngine").c_str());

    report(100, L"Done.");
    result.success = true;
    return result;
}

}  // namespace ce::setup
