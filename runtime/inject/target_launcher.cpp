#include "target_launcher.h"

#include <windows.h>

#include <cctype>
#include <memory>
#include <string>
#include <vector>

#include "common/logging/logging.h"
#include "launch_command.h"

namespace ce::runtime {

TargetLauncher::TargetLauncher(std::unique_ptr<TargetInjector> injector) : injector_(std::move(injector)) {}

TargetLauncher::~TargetLauncher() = default;

// Launch game suspended and inject immediately (The only way to guarantee API
// overrides) If the target looks like a launcher (not the actual game exe), we
// just start it normally and let WMI + CreateProcess hooks in already-injected
// processes catch the real game
LaunchResult TargetLauncher::Launch(std::string_view commandLine) {
    LaunchResult result;
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    LaunchCommand launchCommand = {};
    if (!ParseLaunchCommand(commandLine, &launchCommand)) {
        LogError("[Launcher] Failed to parse launch command: %s", std::string(commandLine).c_str());
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }

    // The command is UTF-8; the process is created through the wide API so a path outside the
    // active code page (a Japanese game folder on a Western system, a Cyrillic user name) names
    // the real file instead of a '?'-mangled one.
    std::wstring widePath;
    std::wstring wideWorkingDirectory;
    std::wstring wideCommandLine;
    if (!Utf8ToWide(launchCommand.executablePath, &widePath) ||
        !Utf8ToWide(launchCommand.workingDirectory, &wideWorkingDirectory) ||
        !Utf8ToWide(launchCommand.rawCommandLine, &wideCommandLine)) {
        LogError("[Launcher] Launch command is not valid UTF-8: %s", launchCommand.rawCommandLine.c_str());
        result.error = ERROR_NO_UNICODE_TRANSLATION;
        return result;
    }

    // CreateProcessW may modify its command line buffer.
    std::vector<wchar_t> commandLineBuffer(wideCommandLine.begin(), wideCommandLine.end());
    commandLineBuffer.push_back(L'\0');
    LPWSTR mutableCommandLine = commandLineBuffer.data();
    LPCWSTR workingDir = wideWorkingDirectory.empty() ? NULL : wideWorkingDirectory.c_str();

    const std::wstring& cleanPath = widePath;

    // Extract filename
    std::string filename = launchCommand.fileName;

    // Convert to lowercase
    std::string lowerName;
    for (char c : filename)
        lowerName += (char)tolower(c);

    // Check if this is likely a launcher (not the game itself)
    // Heuristic: if filename doesn't contain _dx11, _dx12, _vulkan, etc., it
    // Check if this looks like the actual game vs a launcher
    // Games typically have: _dx, _vulkan, _vk, game, test, or are known
    // executables
    bool looksLikeGame =
        (lowerName.find("_dx") != std::string::npos || lowerName.find("_vulkan") != std::string::npos ||

         lowerName.find("_vk") != std::string::npos || lowerName.find("game") != std::string::npos ||
         lowerName.find("_test") != std::string::npos || lowerName.find("test.exe") != std::string::npos);
    bool looksLikeLauncher = !looksLikeGame;

    if (looksLikeLauncher) {
        // This looks like a launcher - start it NORMALLY, no injection
        // We'll rely on WMI to catch the actual game
        LogInfo("[Launcher] Detected launcher (not game): %s - Starting normally",
                launchCommand.rawCommandLine.c_str());

        if (CreateProcessW(cleanPath.c_str(), mutableCommandLine, NULL, NULL, FALSE, 0, NULL, workingDir, &si, &pi)) {
            LogInfo("[Launcher] Launcher started (PID: %lu). WMI will catch the game.", pi.dwProcessId);
            result.started = true;
            result.processId = pi.dwProcessId;
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            result.error = GetLastError();
            LogError("[Launcher] Failed to start launcher: %lu", static_cast<unsigned long>(result.error));
        }
    } else {
        // This looks like the actual game - use suspended injection
        result.kind = LaunchKind::Game;
        LogInfo("[Launcher] Detected game: %s - Launching Suspended", launchCommand.rawCommandLine.c_str());

        if (CreateProcessW(cleanPath.c_str(), mutableCommandLine, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, workingDir,
                           &si, &pi)) {
            result.started = true;
            result.processId = pi.dwProcessId;
            LogInfo(
                "[Launcher] Process Created (PID: %lu). Attempting early APC "
                "injection...",
                pi.dwProcessId);

            {
                // Try early APC injection first (before application startup). The
                // injector selects the hook DLL for the target's architecture from its
                // own UTF-16 install path.
                bool injected = injector_ && injector_->InjectEarly(pi.dwProcessId, pi.hThread);

                if (injected) {
                    LogInfo("[Launcher] Early APC injection successful. Resuming thread.");
                    ResumeThread(pi.hThread);
                    result.injection = LaunchInjection::EarlyApc;
                } else {
                    LogInfo(
                        "[Launcher] APC injection failed, falling back to "
                        "CreateRemoteThread...");
                    ResumeThread(pi.hThread);

                    // Fallback to traditional injection. No delay is needed first: every thread created in the
                    // process, this remote LoadLibrary thread included, starts in LdrInitializeThunk, which waits
                    // until the initial thread has finished process loader initialization before its start
                    // routine runs, so the remote thread serializes behind process initialization by itself.
                    if (injector_ && injector_->InjectResumed(pi.dwProcessId, launchCommand.fileName)) {
                        LogInfo("[Launcher] Fallback injection successful.");
                        result.injection = LaunchInjection::Fallback;
                    } else {
                        LogError(
                            "[Launcher] Fallback injection FAILED. Game running without "
                            "hooks.");
                        result.injection = LaunchInjection::Failed;
                    }
                }
            }

            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            result.error = GetLastError();
            LogError("[Launcher] Failed to CreateProcess: %lu", static_cast<unsigned long>(result.error));
        }
    }
    return result;
}

}  // namespace ce::runtime
