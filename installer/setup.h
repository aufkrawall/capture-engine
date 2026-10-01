#pragma once

// Shared declarations for the Capture Engine setup program and its uninstaller.
//
// Two executables are built from these sources. The setup stub carries the
// payload; captureengine_uninstall.exe (CE_UNINSTALLER) ships inside it and
// lands next to the program. Both use only Windows system DLLs - no runtime,
// no framework, no third-party code.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "install_policy.h"
#include "payload_format.h"

namespace ce::setup {

inline constexpr wchar_t kProductName[] = L"Capture Engine";
inline constexpr wchar_t kPublisher[] = L"aufkrawall";
inline constexpr wchar_t kHomepage[] = L"https://github.com/aufkrawall/capture-engine";
inline constexpr wchar_t kInstallFolderName[] = L"Capture Engine";
inline constexpr wchar_t kAppExe[] = L"captureengine.exe";
inline constexpr wchar_t kUninstallerExe[] = L"captureengine_uninstall.exe";
inline constexpr wchar_t kManifestFile[] = L"captureengine_install.manifest";
inline constexpr wchar_t kShortcutName[] = L"Capture Engine.lnk";
inline constexpr wchar_t kServiceName[] = L"CaptureEngineElevation";
inline constexpr wchar_t kTrayWindowClass[] = L"CaptureEngineTray";
// Written into the Installed Apps record so a foreign entry under the same key is never mistaken for ours.
inline constexpr wchar_t kOwnRecordMarker[] = L"CaptureEngineSetupRecord";
inline constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CaptureEngine";
// Defined in version.cpp: the only unit that includes the per-build version header,
// so a version bump recompiles one file instead of the whole installer.
extern const wchar_t kVersion[];
extern const char kVersionUtf8[];
inline constexpr wchar_t kStagingSuffixWide[] = L".cenew";
inline constexpr wchar_t kBackupSuffixWide[] = L".cebak";
inline constexpr int kIconResource = 101;

// Exit codes. Documented because deployment scripts branch on them.
inline constexpr int kExitOk = 0;
inline constexpr int kExitFailed = 1;
inline constexpr int kExitCancelled = 2;
inline constexpr int kExitBadArguments = 3;

// ---------------------------------------------------------------------------
// Logging (util.cpp). A bounded transcript is kept for every run so a support
// request can be answered from one file; it carries paths and Windows codes
// only, never credentials.
// ---------------------------------------------------------------------------

void LogOpen(const wchar_t* name);
[[gnu::format(printf, 1, 2)]] void Log(const char* format, ...);
const std::wstring& LogFilePath();

// ---------------------------------------------------------------------------
// Strings, paths, files (util.cpp)
// ---------------------------------------------------------------------------

std::wstring Widen(std::string_view utf8);
std::string Narrow(std::wstring_view wide);
std::wstring ErrorText(DWORD error);

std::wstring ModulePath();
std::wstring DirectoryOf(const std::wstring& path);
std::wstring LeafOf(const std::wstring& path);
// Joins with a backslash; `child` may use forward slashes (payload paths).
std::wstring JoinPath(const std::wstring& parent, std::wstring_view child);
std::wstring KnownFolder(const GUID& id);
std::wstring ProgramFilesDirectory();
std::wstring WindowsDirectory();
bool PathExists(const std::wstring& path);
bool DirectoryExists(const std::wstring& path);
// The deepest existing folder on `path`, for free-space queries on a folder not yet created.
std::wstring NearestExistingDirectory(std::wstring path);
bool CreateDirectoryTree(const std::wstring& path, DWORD* error);
bool ReadWholeFile(const std::wstring& path, std::string* contents, uint64_t limit);
// Writes through a staging file and an atomic rename; never leaves a partial file.
bool WriteFileAtomically(const std::wstring& path, const void* data, size_t size, bool replaceExisting, DWORD* error);
std::wstring ProcessImagePath(DWORD processId);
bool IsProcessElevated();
bool IsProcessAlive(DWORD processId);

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE handle) : handle_(handle) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : handle_(other.Release()) {}
    Handle& operator=(Handle&& other) noexcept {
        Reset(other.Release());
        return *this;
    }
    HANDLE Get() const { return handle_; }
    bool Valid() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
    HANDLE Release() {
        HANDLE result = handle_;
        handle_ = nullptr;
        return result;
    }
    void Reset(HANDLE handle = nullptr) {
        if (Valid())
            CloseHandle(handle_);
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

// ---------------------------------------------------------------------------
// Payload (payload.cpp)
// ---------------------------------------------------------------------------

class PayloadReader {
public:
    PayloadReader() = default;
    // Reads and validates footer and index of `exePath`. Does not read file data.
    PayloadStatus Open(const std::wstring& exePath);
    const std::vector<FileEntry>& Files() const { return files_; }
    const FileEntry* Find(std::string_view path) const;
    uint64_t TotalSize() const { return footer_.totalSize; }
    // Streams one file into `destination` (created new), verifying its CRC.
    // `progress` receives the number of content bytes written so far.
    PayloadStatus ExtractFile(const FileEntry& entry, const std::wstring& destination, DWORD* win32Error,
                              const std::function<void(uint64_t)>& progress = {});
    PayloadStatus ReadContents(const FileEntry& entry, std::string* contents, DWORD* win32Error);
    // Decodes every file without writing it; the integrity check behind --verify-payload.
    PayloadStatus VerifyAll(DWORD* win32Error);

private:
    using Sink = std::function<bool(const uint8_t*, size_t)>;
    PayloadStatus DecodeFile(const FileEntry& entry, const Sink& sink, DWORD* win32Error,
                             const std::function<void(uint64_t)>& progress);

    Handle file_;
    Footer footer_;
    std::vector<FileEntry> files_;
};

// ---------------------------------------------------------------------------
// Installed files (files.cpp): shared by setup and uninstaller
// ---------------------------------------------------------------------------

bool IsReparsePoint(const std::wstring& path);
// True when a folder between the installation root and `relative` is a reparse
// point (junction, symlink), which would redirect an elevated write or delete
// outside the installation.
bool HasReparseAncestor(const std::wstring& directory, const std::string& relative);
bool LoadManifest(const std::wstring& directory, Manifest* manifest);
// Deletes `path`. A file that is in use (a hook DLL loaded into a running game)
// is renamed aside and scheduled for deletion at the next restart. True when
// the original name is free afterwards.
bool RemoveOrRenameAway(const std::wstring& path, bool* deferredToReboot);
void RemoveManifestFiles(const std::wstring& directory, const std::vector<std::string>& files,
                         std::vector<std::wstring>* warnings, bool* rebootRecommended);
// Removes each listed directory only if it is empty; never recursive.
void RemoveEmptyDirectories(const std::wstring& directory, const std::vector<std::string>& files);
// Deletes leftovers of an interrupted run (.cenew / .cebak) in the listed folders.
void CleanTemporaryFiles(const std::wstring& directory, const std::vector<std::string>& files);

// ---------------------------------------------------------------------------
// Installation state and actions
// ---------------------------------------------------------------------------

using ProgressFn = std::function<void(int percent, const std::wstring& status)>;

struct ExistingInstall {
    bool registered = false;
    std::wstring directory;
    std::wstring version;
    ExistingState state;
};

// Reads the uninstall record and, for the interactive user, the stored startup
// preferences; `ownerSid` empty means "figure out from the shell".
ExistingInstall ReadExistingInstall(const std::wstring& candidateDirectory);

struct InstallRequest {
    std::wstring directory;
    uint32_t options = 0;
    // Testing: only the file transaction and config.ini handling run.
    bool filesOnly = false;
    unsigned closeTimeoutSeconds = 30;
};

struct InstallResult {
    bool success = false;
    // Files are in place and registered even when a warning is present.
    std::vector<std::wstring> warnings;
    std::wstring error;
    bool configWritten = false;
    bool templateWritten = false;
    bool rebootRecommended = false;
    bool pawnIoInstalled = false;
    bool serviceEnabled = false;
    bool startupConfigured = false;
    bool launched = false;
};

InstallResult RunInstall(PayloadReader& payload, const InstallRequest& request, const ProgressFn& progress);

struct UninstallRequest {
    std::wstring directory;
    bool removeUserData = false;
    bool filesOnly = false;
    unsigned closeTimeoutSeconds = 30;
};

struct UninstallResult {
    bool success = false;
    std::vector<std::wstring> warnings;
    std::wstring error;
    bool rebootRecommended = false;
};

UninstallResult RunUninstall(const UninstallRequest& request, const ProgressFn& progress);

// ---------------------------------------------------------------------------
// Running instances (process_control.cpp)
// ---------------------------------------------------------------------------

// Asks every Capture Engine process running from `directory` to exit, waits for
// them, and stops the elevation service. Returns false (with a reason) only
// when something still runs from the folder afterwards.
bool CloseRunningInstances(const std::wstring& directory, std::wstring* error, const ProgressFn& progress,
                           bool stopService = true, unsigned graceSeconds = 30);
bool StopElevationService(std::wstring* error);
bool ElevationServiceExists();

// ---------------------------------------------------------------------------
// Registration, shortcuts, permissions, helpers (registration.cpp, integration.cpp)
// ---------------------------------------------------------------------------

bool WriteUninstallRecord(const std::wstring& directory, uint64_t estimatedBytes, DWORD* error);
bool RemoveUninstallRecord();
bool CreateAppShortcut(const std::wstring& linkPath, const std::wstring& directory, DWORD* error);
// Removes the link only when it targets this installation. Returns true if absent afterwards.
bool RemoveOwnedShortcut(const std::wstring& linkPath, const std::wstring& directory);
bool OwnedShortcutExists(const std::wstring& linkPath, const std::wstring& directory);
std::wstring StartMenuShortcutPath();
std::wstring DesktopShortcutPath();
// Lets ordinary users write the folders and file the program writes at runtime.
bool GrantUsersModify(const std::wstring& path, bool inheritToChildren, DWORD* error);

// The interactive user's shell process: whose desktop, registry hive and
// startup entries the installation is for.
DWORD InteractiveShellProcess();
bool LaunchAsInteractiveUser(const std::wstring& exePath, const std::wstring& arguments, const std::wstring& workDir);

struct IntegrationRequest {
    std::wstring directory;
    uint32_t options = 0;
    bool applyService = false;
    bool applyAutostart = false;
    bool removeService = false;
    std::wstring ownerSid;
    bool ownerAdministrator = false;
};

// Runs the installed captureengine.exe's elevated setup role and returns its
// Win32 exit code (0 = success).
DWORD RunIntegrationRole(const IntegrationRequest& request);
bool ResolveOwner(std::wstring* sid, bool* administrator);
std::wstring RegisteredServiceOwner();
DWORD RunPawnIoRole(const std::wstring& directory);
bool IsPawnIoInstalled();

}  // namespace ce::setup
