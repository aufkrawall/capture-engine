#include "main_internal.h"

namespace {
std::string TrimCommandWhitespace(const std::string& value) {
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }

    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}
}

namespace {
bool ParseDeferredLaunchCommand(const std::string& command, DeferredLaunchCommand* outCommand) {
    if (!outCommand) {
        return false;
    }

    *outCommand = {};
    outCommand->rawCommandLine = TrimCommandWhitespace(command);
    if (outCommand->rawCommandLine.empty()) {
        return false;
    }

    const std::string& raw = outCommand->rawCommandLine;
    if (raw.front() == '"') {
        const size_t closingQuote = raw.find('"', 1);
        if (closingQuote == std::string::npos || closingQuote == 1) {
            return false;
        }
        outCommand->executablePath = raw.substr(1, closingQuote - 1);
    } else {
        const size_t separator = raw.find_first_of(" \t\r\n");
        outCommand->executablePath = raw.substr(0, separator);
    }

    if (outCommand->executablePath.empty()) {
        return false;
    }

    const size_t lastSlash = outCommand->executablePath.find_last_of("\\/");
    outCommand->fileName = (lastSlash != std::string::npos) ? outCommand->executablePath.substr(lastSlash + 1)
                                                            : outCommand->executablePath;
    if (lastSlash != std::string::npos) {
        outCommand->workingDirectory = outCommand->executablePath.substr(0, lastSlash);
    }

    return true;
}
}

// Launch game suspended and inject immediately (The only way to guarantee API
// overrides) If the target looks like a launcher (not the actual game exe), we
// just start it normally and let WMI + CreateProcess hooks in already-injected
// processes catch the real game
void LaunchGameSuspended(const std::string& path) {
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    DeferredLaunchCommand launchCommand = {};
    if (!ParseDeferredLaunchCommand(path, &launchCommand)) {
        LogError("[Launcher] Failed to parse launch command: %s", path.c_str());
        return;
    }

    std::vector<char> commandLineBuffer(launchCommand.rawCommandLine.begin(), launchCommand.rawCommandLine.end());
    commandLineBuffer.push_back('\0');
    LPSTR mutableCommandLine = commandLineBuffer.data();
    LPCSTR workingDir = launchCommand.workingDirectory.empty() ? NULL : launchCommand.workingDirectory.c_str();

    const std::string& cleanPath = launchCommand.executablePath;

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

        if (CreateProcessA(cleanPath.c_str(), mutableCommandLine, NULL, NULL, FALSE, 0, NULL, workingDir, &si, &pi)) {
            LogInfo("[Launcher] Launcher started (PID: %lu). WMI will catch the game.", pi.dwProcessId);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            LogError("[Launcher] Failed to start launcher: %lu", GetLastError());
        }
    } else {
        // This looks like the actual game - use suspended injection
        LogInfo("[Launcher] Detected game: %s - Launching Suspended", launchCommand.rawCommandLine.c_str());

        if (CreateProcessA(cleanPath.c_str(), mutableCommandLine, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, workingDir,
                           &si, &pi)) {
            LogInfo(
                "[Launcher] Process Created (PID: %lu). Attempting early APC "
                "injection...",
                pi.dwProcessId);

            auto injector = std::make_shared<InjectionManager>(main_g_Config);

            // Determine DLL path based on target architecture
            BOOL isWow64 = FALSE;
            HANDLE hCheckProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pi.dwProcessId);
            if (hCheckProcess) {
                IsWow64Process(hCheckProcess, &isWow64);
                CloseHandle(hCheckProcess);
            }

            std::string hookDllPath;
            char buffer[MAX_PATH];
            const DWORD modulePathChars = GetModuleFileNameA(NULL, buffer, MAX_PATH);
            const size_t lastSeparator =
                (modulePathChars > 0 && modulePathChars < MAX_PATH)
                    ? std::string(buffer).find_last_of("\\/")
                    : std::string::npos;
            if (modulePathChars == 0 || modulePathChars >= MAX_PATH || lastSeparator == std::string::npos) {
                LogError("[Launcher] Cannot resolve the application directory reliably (chars=%lu); resuming %s "
                         "without injection",
                         static_cast<unsigned long>(modulePathChars), launchCommand.fileName.c_str());
                ResumeThread(pi.hThread);
            } else {
                const std::string baseDir = std::string(buffer).substr(0, lastSeparator);
                hookDllPath = isWow64 ? (baseDir + "\\capture_hook_x86.dll") : (baseDir + "\\capture_hook_x64.dll");

                // Try early APC injection first (runs before import resolution)
                bool injected = injector->InjectEarly(pi.dwProcessId, hookDllPath, pi.hThread);

                if (injected) {
                    LogInfo("[Launcher] Early APC injection successful. Resuming thread.");
                    ResumeThread(pi.hThread);
                } else {
                    LogInfo(
                        "[Launcher] APC injection failed, falling back to "
                        "CreateRemoteThread...");
                    ResumeThread(pi.hThread);

                    // Fallback to traditional injection
                    Sleep(100);  // Give process a moment to initialize
                    if (injector->Inject(pi.dwProcessId, launchCommand.fileName)) {
                        LogInfo("[Launcher] Fallback injection successful.");
                    } else {
                        LogError(
                            "[Launcher] Fallback injection FAILED. Game running without "
                            "hooks.");
                    }
                }
            }

            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            LogError("[Launcher] Failed to CreateProcess: %lu", GetLastError());
        }
    }
}

// Spawn authenticates IPC synchronously; there is no reconnect-by-name phase.
bool ConnectToChildProcesses(DWORD) {
    return (!main_g_hInjectProcess || main_g_InjectClient->IsConnected()) && (!main_g_hMediaProcess || main_g_MediaClient->IsConnected()) &&
           (!main_g_hLimiterProcess || main_g_LimiterClient->IsConnected());
}

// Send command to all child processes
void SendCommandToAll(ProcessCommand cmd) {
    if (main_g_InjectClient && main_g_InjectClient->IsConnected()) {
        main_g_InjectClient->SendCommand(cmd);
    }
    if (main_g_MediaClient && main_g_MediaClient->IsConnected()) {
        main_g_MediaClient->SendCommand(cmd);
    }
    if (main_g_LimiterClient && main_g_LimiterClient->IsConnected()) {
        main_g_LimiterClient->SendCommand(cmd);
    }
}
