// Entry point of the Capture Engine setup program and its uninstaller.
//
// CE_UNINSTALLER selects the removal-only build that ships inside the payload.
// Both binaries are asInvoker and elevate themselves when a mode needs it, so
// the read-only modes (--verify-payload, --extract, --preview) run without a
// prompt and can be exercised by tools.

#include "wizard.h"
#include "common/ipc/elevation_windows.h"

#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <map>
#include <optional>

namespace ce::setup {
namespace {

#if defined(CE_UNINSTALLER)
constexpr wchar_t kLogName[] = L"CaptureEngine-Uninstall.log";
std::wstring Usage() {
    return std::wstring(L"Capture Engine ") + kVersion +
           L" uninstaller\n\n"
           L"  /S, --silent    Remove Capture Engine without a window.\n"
           L"  --remove-data   Also delete config.ini and the logs folder.\n"
           L"  /?, --help      Show this text.\n\n"
           L"Exit codes: 0 success, 1 failure, 2 cancelled, 3 bad arguments.";
}
#else
constexpr wchar_t kLogName[] = L"CaptureEngine-Setup.log";
std::wstring Usage() {
    return std::wstring(L"Capture Engine ") + kVersion +
           L" setup\n\n"
           L"  /S, --silent               Install or update without a window.\n"
           L"  /D=<path>, --dir=<path>    Install into <path> (the Capture Engine folder itself).\n"
           L"  --desktop, --no-desktop    Desktop shortcut.\n"
           L"  --start-menu, --no-start-menu\n"
           L"  --autostart, --no-autostart  Start when signing in.\n"
           L"  --service, --no-service    Elevation service.\n"
           L"  --admin, --no-admin        Always run as administrator.\n"
           L"  --pawnio, --no-pawnio      PawnIO driver.\n"
           L"  --launch, --no-launch      Start Capture Engine afterwards.\n"
           L"  --uninstall                Remove an existing installation.\n"
           L"  --remove-data              With --uninstall: also delete config.ini and logs.\n"
           L"  --extract=<new folder>     Unpack the files only; nothing is registered.\n"
           L"  --verify-payload           Check this setup file's integrity.\n"
           L"  /?, --help                 Show this text.\n\n"
           L"Exit codes: 0 success, 1 failure, 2 cancelled, 3 bad arguments.";
}
#endif

std::vector<std::wstring> CommandLineArguments() {
    std::vector<std::wstring> arguments;
    int count = 0;
    LPWSTR* parsed = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!parsed)
        return arguments;
    for (int index = 1; index < count; ++index)
        arguments.emplace_back(parsed[index]);
    LocalFree(reinterpret_cast<HLOCAL>(parsed));
    return arguments;
}

// Everything after the program name, exactly as typed, for the elevated copy.
std::wstring RawArgumentTail() {
    std::wstring_view command(GetCommandLineW());
    size_t end = 0;
    if (!command.empty() && command.front() == L'"') {
        end = command.find(L'"', 1);
        if (end != std::wstring_view::npos)
            ++end;
    } else {
        end = command.find_first_of(L" \t");
    }
    return end == std::wstring_view::npos ? std::wstring() : std::wstring(command.substr(end));
}

int RelaunchElevated() {
    const std::wstring self = ModulePath();
    const std::wstring arguments = RawArgumentTail() + L" --elevation-launcher=" +
                                   std::to_wstring(GetCurrentProcessId());
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = self.c_str();
    info.lpParameters = arguments.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        const DWORD error = GetLastError();
        Log("elevate: %s", Narrow(ErrorText(error)).c_str());
        return error == ERROR_CANCELLED ? kExitCancelled : kExitFailed;
    }
    Handle child(info.hProcess);
    WaitForSingleObject(child.Get(), INFINITE);
    DWORD code = kExitFailed;
    GetExitCodeProcess(child.Get(), &code);
    return static_cast<int>(code);
}

#if !defined(CE_UNINSTALLER)
std::wstring DefaultDirectory() {
    const std::wstring programFiles = ProgramFilesDirectory();
    return JoinPath(programFiles.empty() ? std::wstring(L"C:\\Program Files") : programFiles, kInstallFolderName);
}
#endif

// The uninstaller lives inside the folder it removes. Running it from there
// would keep its own image locked, so it copies itself to a temporary folder
// and waits for the copy's real result. Waiting launchers are excluded from
// shutdown; their mapped image can be renamed aside during removal.
std::optional<int> ContinueFromTemporaryCopy(const CommandLine& command, const std::wstring& directory) {
    const std::wstring self = ModulePath();
    wchar_t temp[MAX_PATH + 2] = {};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (!length || length >= MAX_PATH)
        return std::nullopt;
    GUID nonce{};
    wchar_t nonceText[40] = {};
    if (FAILED(CoCreateGuid(&nonce)) || !StringFromGUID2(nonce, nonceText, 40))
        return std::nullopt;
    const std::wstring folder = std::wstring(temp, length) + L"CaptureEngine-Uninstall-" + nonceText;
    if (!CreateDirectoryW(folder.c_str(), nullptr))
        return std::nullopt;
    const std::wstring copy = JoinPath(folder, kUninstallerExe);
    if (!CopyFileW(self.c_str(), copy.c_str(), TRUE)) {
        RemoveDirectoryW(folder.c_str());
        return std::nullopt;
    }
    std::wstring arguments = L"--uninstall --from-temporary-copy --dir=\"" + directory + L"\" --wait-process=" +
                             std::to_wstring(GetCurrentProcessId()) + L" --close-timeout=" +
                             std::to_wstring(command.closeTimeoutSeconds);
    if (command.silent)
        arguments += L" /S";
    if (command.removeUserData)
        arguments += L" --remove-data";
    if (command.filesOnly)
        arguments += L" --files-only";
    if (!command.elevationLauncher.empty())
        arguments += L" --elevation-launcher=" + ce::elevation::QuoteArgument(command.elevationLauncher);
    std::wstring commandLine = L"\"" + copy + L"\" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Log("uninstall: handing removal to a temporary copy and waiting for its result");
    LogClose();  // The child owns the transcript while it works.
    if (!CreateProcessW(copy.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup,
                        &process)) {
        Log("uninstall: cannot start the temporary copy (error %lu)", GetLastError());
        DeleteFileW(copy.c_str());
        RemoveDirectoryW(folder.c_str());
        return std::nullopt;
    }
    Handle thread(process.hThread);
    Handle child(process.hProcess);
    DWORD code = kExitFailed;
    if (WaitForSingleObject(child.Get(), INFINITE) != WAIT_OBJECT_0 || !GetExitCodeProcess(child.Get(), &code))
        Log("uninstall: could not obtain the temporary copy's result (error %lu)", GetLastError());
    // The child has exited: its image can be removed immediately, with no helper
    // process or temp executable left running until restart.
    if (!DeleteFileW(copy.c_str()) || !RemoveDirectoryW(folder.c_str()))
        Log("uninstall: temporary copy cleanup failed (error %lu)", GetLastError());
    Log("uninstall: temporary copy finished with exit code %lu", static_cast<unsigned long>(code));
    return static_cast<int>(code);
}

// The internal PID must identify this copy's actual live parent. Retain handles
// to it and any same-image UAC launcher ancestors so PID reuse cannot exempt a
// different process from shutdown. Other Capture Engine processes still close.
std::vector<Handle> OpenWaitingInstallers(const std::wstring& parentArgument,
                                        const std::wstring& elevationLauncher, std::vector<DWORD>* ids) {
    std::vector<Handle> handles;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    std::map<DWORD, DWORD> parents;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!snapshot.Valid())
        return handles;
    for (BOOL more = Process32FirstW(snapshot.Get(), &entry); more; more = Process32NextW(snapshot.Get(), &entry))
        parents[entry.th32ProcessID] = entry.th32ParentProcessID;
    DWORD pid = parents[GetCurrentProcessId()];
    if (!pid || parentArgument != std::to_wstring(pid))
        return handles;
    const std::wstring image = ProcessImagePath(pid);
    FILETIME before{}, exited{}, kernel{}, user{};
    if (image.empty() || !GetProcessTimes(GetCurrentProcess(), &before, &exited, &kernel, &user))
        return handles;
    FILETIME parentCreated{};
    while (pid && EqualsNoCase(ProcessImagePath(pid), image)) {
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
        FILETIME created{};
        if (!process.Valid() || WaitForSingleObject(process.Get(), 0) != WAIT_TIMEOUT ||
            !GetProcessTimes(process.Get(), &created, &exited, &kernel, &user) || CompareFileTime(&created, &before) >= 0)
            break;
        if (handles.empty())
            parentCreated = created;
        before = created;
        ids->push_back(pid);
        handles.push_back(std::move(process));
        pid = parents[pid];
    }
    if (!elevationLauncher.empty()) {
        bool retained = false;
        // Compare only against PIDs from the OS snapshot: no numeric parsing of
        // the internal argument, overflow or recycled older ancestor is accepted.
        for (const auto& processEntry : parents) {
            const DWORD candidate = processEntry.first;
            if (elevationLauncher != std::to_wstring(candidate))
                continue;
            if (std::find(ids->begin(), ids->end(), candidate) != ids->end()) {
                retained = true;
                break;
            }
            Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, candidate));
            FILETIME created{};
            if (process.Valid() && WaitForSingleObject(process.Get(), 0) == WAIT_TIMEOUT &&
                GetProcessTimes(process.Get(), &created, &exited, &kernel, &user) &&
                CompareFileTime(&created, &parentCreated) < 0 && EqualsNoCase(ProcessImagePath(candidate), image)) {
                ids->push_back(candidate);
                handles.push_back(std::move(process));
                retained = true;
            }
            break;
        }
        if (!retained) {
            Log("uninstall: could not validate the initiating UAC launcher");
            ids->clear();
            handles.clear();
            return handles;
        }
    }
    Log("uninstall: retaining %zu waiting installer launcher(s) during removal", handles.size());
    return handles;
}

int RunUninstallMode(HINSTANCE instance, const CommandLine& command) {
    std::wstring directory = NormalizeDirectory(command.directory);
#if defined(CE_UNINSTALLER)
    if (directory.empty())
        directory = DirectoryOf(ModulePath());
#else
    if (directory.empty()) {
        const ExistingInstall existing = ReadExistingInstall(std::wstring());
        directory = NormalizeDirectory(existing.directory);
    }
#endif
    if (directory.empty()) {
        const std::wstring message = L"No Capture Engine installation was found to remove.";
        Log("uninstall: %s", Narrow(message).c_str());
        if (!command.silent)
            ShowMessage(nullptr, message, kProductName, true);
        return kExitFailed;
    }
    if (!command.fromTemporaryCopy && IsPathInside(ModulePath(), directory)) {
        if (const auto code = ContinueFromTemporaryCopy(command, directory))
            return *code;
        Log("uninstall: continuing without a temporary copy; the uninstaller itself may remain");
    }
    std::vector<DWORD> waitingInstallers;
    const auto launchers = command.fromTemporaryCopy ? OpenWaitingInstallers(command.waitProcess, command.elevationLauncher, &waitingInstallers)
                                                    : std::vector<Handle>();
    if (command.fromTemporaryCopy && launchers.empty()) {
        Log("uninstall: the temporary copy has no valid waiting launcher");
        return kExitBadArguments;
    }
    int code;
    if (command.silent) {
        UninstallRequest request;
        request.directory = directory;
        request.removeUserData = command.removeUserData;
        request.filesOnly = command.filesOnly;
        request.waitingInstallers = waitingInstallers;
        request.closeTimeoutSeconds = command.closeTimeoutSeconds;
        const UninstallResult result = RunUninstall(request, {});
        if (!result.success)
            Log("uninstall failed: %s", Narrow(result.error).c_str());
        code = result.success ? kExitOk : kExitFailed;
    } else {
        code = RunUninstallWizard(instance, directory, command.removeUserData, command.filesOnly,
                                  command.closeTimeoutSeconds, waitingInstallers);
    }
    return code;
}

#if !defined(CE_UNINSTALLER)

int ExtractOnly(PayloadReader& payload, const std::wstring& target) {
    const std::wstring directory = NormalizeDirectory(target);
    if (ValidateInstallDirectory(directory, WindowsDirectory()) != DirectoryIssue::Ok) {
        Log("extract: %s is not a usable folder", Narrow(directory).c_str());
        return kExitBadArguments;
    }
    if (DirectoryExists(directory)) {
        WIN32_FIND_DATAW data{};
        HANDLE search = FindFirstFileW(JoinPath(directory, L"*").c_str(), &data);
        if (search != INVALID_HANDLE_VALUE) {
            bool empty = true;
            do {
                if (wcscmp(data.cFileName, L".") != 0 && wcscmp(data.cFileName, L"..") != 0)
                    empty = false;
            } while (empty && FindNextFileW(search, &data));
            FindClose(search);
            if (!empty) {
                Log("extract: %s is not empty", Narrow(directory).c_str());
                return kExitBadArguments;
            }
        }
    }
    DWORD error = 0;
    for (const FileEntry& entry : payload.Files()) {
        const std::wstring destination = JoinPath(directory, Widen(entry.path));
        if (!CreateDirectoryTree(DirectoryOf(destination), &error))
            return kExitFailed;
        const PayloadStatus status = payload.ExtractFile(entry, destination, &error);
        if (status != PayloadStatus::Ok) {
            Log("extract: %s: %s", entry.path.c_str(), PayloadStatusText(status));
            return kExitFailed;
        }
    }
    Log("extract: %zu files written", payload.Files().size());
    return kExitOk;
}

int SilentInstall(PayloadReader& payload, const CommandLine& command) {
    const std::wstring defaultDirectory = DefaultDirectory();
    const ExistingInstall existing = ReadExistingInstall(defaultDirectory);
    InstallRequest request;
    request.directory = !command.directory.empty() ? command.directory
                        : existing.registered && DirectoryExists(existing.directory) ? existing.directory
                                                                                    : defaultDirectory;
    // A silent install does not start the program unless asked to.
    request.options = ApplyOverrides(DefaultOptions(existing.state) & ~static_cast<uint32_t>(kOptLaunch), command);
    request.closeTimeoutSeconds = command.closeTimeoutSeconds;
    const InstallResult result = RunInstall(payload, request, {});
    if (!result.success) {
        Log("silent install failed: %s", Narrow(result.error).c_str());
        return kExitFailed;
    }
    if (request.options & kOptLaunch)
        LaunchAsInteractiveUser(JoinPath(NormalizeDirectory(request.directory), kAppExe), L"",
                                NormalizeDirectory(request.directory));
    return kExitOk;
}

#endif  // !CE_UNINSTALLER

// Testing only: the file transaction without anything outside the folder (no
// registry, service, shortcuts, driver or elevation), so the update, rollback and
// config.ini rules can be exercised on a scratch directory.
int RunFilesOnly(const CommandLine& command) {
    if (command.directory.empty()) {
        Log("files-only needs --dir=<folder>");
        return kExitBadArguments;
    }
#if !defined(CE_UNINSTALLER)
    PayloadReader payload;
    const PayloadStatus status = payload.Open(ModulePath());
    if (status != PayloadStatus::Ok) {
        Log("payload: %s", PayloadStatusText(status));
        return kExitFailed;
    }
    InstallRequest request;
    request.directory = command.directory;
    request.filesOnly = true;
    request.closeTimeoutSeconds = command.closeTimeoutSeconds;
    const InstallResult result = RunInstall(payload, request, {});
    if (!result.success)
        Log("files-only install failed: %s", Narrow(result.error).c_str());
    return result.success ? kExitOk : kExitFailed;
#else
    return kExitBadArguments;
#endif
}

int Run(HINSTANCE instance) {
    const CommandLine command = ParseCommandLine(CommandLineArguments());
#if defined(CE_UNINSTALLER)
    CommandLine effective = command;
    if (effective.valid && effective.mode == Mode::Install)
        effective.mode = Mode::Uninstall;
    const CommandLine& active = effective;
#else
    const CommandLine& active = command;
#endif
    // The copy that only asks for elevation must not open the log: it stays alive while
    // the elevated copy works, and a file held open for writing would keep that copy
    // from writing its own transcript.
    const bool needsElevation = active.valid && (active.mode == Mode::Install || active.mode == Mode::Uninstall) &&
                                !active.filesOnly && !IsProcessElevated();
    if (!needsElevation) {
        LogOpen(kLogName);
        Log("started: version %s, elevated=%d", kVersionUtf8, IsProcessElevated() ? 1 : 0);
    }
    if (!active.valid) {
        ShowMessage(nullptr, Widen(active.error) + L"\n\n" + Usage(), kProductName, true);
        return kExitBadArguments;
    }
    if (active.mode == Mode::Help) {
        ShowMessage(nullptr, Usage(), kProductName, false);
        return kExitOk;
    }

#if !defined(CE_UNINSTALLER)
    // Read-only modes need no elevation.
    if (active.mode == Mode::Preview)
        return RunPreview(instance, NormalizeDirectory(active.previewDirectory));
    if (active.mode == Mode::VerifyPayload || active.mode == Mode::Extract) {
        PayloadReader payload;
        const PayloadStatus status = payload.Open(ModulePath());
        if (status != PayloadStatus::Ok) {
            Log("payload: %s", PayloadStatusText(status));
            return kExitFailed;
        }
        DWORD error = 0;
        if (active.mode == Mode::VerifyPayload) {
            const PayloadStatus verified = payload.VerifyAll(&error);
            Log("verify-payload: %s", PayloadStatusText(verified));
            return verified == PayloadStatus::Ok ? kExitOk : kExitFailed;
        }
        return ExtractOnly(payload, active.extractDirectory);
    }
#endif

    if (active.filesOnly && active.silent && active.mode == Mode::Install)
        return RunFilesOnly(active);

    if (needsElevation)
        return RelaunchElevated();

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int code = kExitFailed;
    if (active.mode == Mode::Uninstall) {
        code = RunUninstallMode(instance, active);
    } else {
#if defined(CE_UNINSTALLER)
        code = kExitBadArguments;
#else
        PayloadReader payload;
        const PayloadStatus status = payload.Open(ModulePath());
        if (status != PayloadStatus::Ok) {
            const std::wstring message = L"This setup file does not contain the Capture Engine files (" +
                                         Widen(PayloadStatusText(status)) + L"). Download it again.";
            Log("payload: %s", PayloadStatusText(status));
            if (!active.silent)
                ShowMessage(nullptr, message, kProductName, true);
            code = kExitFailed;
        } else if (active.silent) {
            code = SilentInstall(payload, active);
        } else {
            code = RunSetupWizard(instance, &payload, active, DefaultDirectory());
        }
#endif
    }
    if (SUCCEEDED(com))
        CoUninitialize();
    Log("finished with exit code %d", code);
    return code;
}

}  // namespace
}  // namespace ce::setup

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Before anything is loaded: only System32 is searched for later LoadLibrary
    // calls, so a DLL planted beside a downloaded setup file is never picked up.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    return ce::setup::Run(instance);
}
