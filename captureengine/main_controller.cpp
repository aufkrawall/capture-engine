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

namespace {

bool TryParseBool(std::string_view val, bool& out) {
    if (val == "1" || _stricmp(std::string(val).c_str(), "true") == 0 ||
        _stricmp(std::string(val).c_str(), "on") == 0 || _stricmp(std::string(val).c_str(), "yes") == 0) {
        out = true;
        return true;
    }
    if (val == "0" || _stricmp(std::string(val).c_str(), "false") == 0 ||
        _stricmp(std::string(val).c_str(), "off") == 0 || _stricmp(std::string(val).c_str(), "no") == 0) {
        out = false;
        return true;
    }
    return false;
}

bool TryParseInt(std::string_view val, int minVal, int maxVal, int& out) {
    char* end = nullptr;
    std::string s(val);
    long v = strtol(s.c_str(), &end, 10);
    if (end != s.c_str() && *end == '\0') {
        out = std::clamp(static_cast<int>(v), minVal, maxVal);
        return true;
    }
    return false;
}

PseudoOverlayConfig ParseProfileDesktopOverlayOverrides(const std::string& path, const std::string& section,
                                                        const PseudoOverlayConfig& base) {
    char buffer[4096];
    const DWORD chars = GetPrivateProfileSectionA(section.c_str(), buffer, sizeof(buffer), path.c_str());
    if (chars == 0 || chars >= sizeof(buffer) - 2)
        return base;

    PseudoOverlayConfig cfg = base;
    for (const char* p = buffer; *p; p += strlen(p) + 1) {
        std::string line(p);
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        key.erase(0, key.find_first_not_of(" \t"));
        const size_t keyEnd = key.find_last_not_of(" \t");
        if (keyEnd != std::string::npos)
            key.erase(keyEnd + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        const size_t valEnd = val.find_last_not_of(" \t");
        if (valEnd != std::string::npos)
            val.erase(valEnd + 1);

        std::string lowerKey;
        lowerKey.reserve(key.size());
        for (char c : key)
            lowerKey.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

        if (lowerKey == "desktopoverlay.enabled" || lowerKey == "pseudo-overlay.enabled") {
            TryParseBool(val, cfg.enabled);
        } else if (lowerKey == "desktopoverlay.size" || lowerKey == "pseudo-overlay.size") {
            TryParseInt(val, 10, 200, cfg.size);
        } else if (lowerKey == "desktopoverlay.pad" || lowerKey == "pseudo-overlay.pad") {
            TryParseInt(val, 0, 100, cfg.pad);
        } else if (lowerKey == "desktopoverlay.pos" || lowerKey == "pseudo-overlay.pos") {
            TryParseInt(val, 0, 3, cfg.pos);
        } else if (lowerKey == "desktopoverlay.mode" || lowerKey == "pseudo-overlay.mode") {
            TryParseInt(val, 0, 2, cfg.mode);
        } else if (lowerKey == "desktopoverlay.always_render" || lowerKey == "pseudo-overlay.always_render" ||
                   lowerKey == "always_render") {
            TryParseBool(val, cfg.alwaysRender);
        } else if (lowerKey == "desktopoverlay.always_render_only_when_game" ||
                   lowerKey == "pseudo-overlay.always_render_only_when_game" ||
                   lowerKey == "always_render_only_when_game") {
            TryParseBool(val, cfg.alwaysRenderOnlyWhenGame);
        } else if (lowerKey == "desktopoverlay.show_encoder_overload_warnings" ||
                   lowerKey == "pseudo-overlay.show_encoder_overload_warnings" ||
                   lowerKey == "show_encoder_overload_warnings") {
            TryParseBool(val, cfg.showEncoderOverloadWarn);
        } else if (lowerKey == "desktopoverlay.foreground_acquire_grace_ms" ||
                   lowerKey == "pseudo-overlay.foreground_acquire_grace_ms" ||
                   lowerKey == "foreground_acquire_grace_ms") {
            TryParseInt(val, 0, 10000, cfg.foregroundAcquireGraceMs);
        }
    }
    return cfg;
}

std::vector<PseudoOverlayApplicationConfig> ResolvePseudoOverlayApplicationConfigs(const AppConfig& baseConfig) {
    std::vector<PseudoOverlayApplicationConfig> profiles;
    profiles.reserve(baseConfig.applicationProfiles.size());

    for (const ApplicationProfile& profile : baseConfig.applicationProfiles) {
        if (!profile.target.HasProcess())
            continue;

        PseudoOverlayApplicationConfig overlayProfile;
        overlayProfile.section = profile.section;
        overlayProfile.processName = profile.target.pattern;
        overlayProfile.settings =
            ParseProfileDesktopOverlayOverrides(main_g_ConfigPath, profile.section, baseConfig.pseudoOverlay);
        overlayProfile.settings.processList = baseConfig.pseudoOverlay.processList;
        overlayProfile.warningTarget = profile.resolvedVideoCapture != ApplicationVideoCapture::kNone;
        overlayProfile.captureUsesInjection =
            profile.resolvedVideoCapture == ApplicationVideoCapture::kInject;
        profiles.push_back(std::move(overlayProfile));
    }

    LogDebug("[Controller] Resolved DesktopOverlay settings for %zu process-backed application profiles",
             profiles.size());
    return profiles;
}

}  // namespace

void SyncPseudoOverlayConfiguration(const char* reason) {
    std::vector<PseudoOverlayApplicationConfig> profiles = ResolvePseudoOverlayApplicationConfigs(main_g_Config);
    const bool anyProfileEnabled =
        std::any_of(profiles.begin(), profiles.end(), [](const PseudoOverlayApplicationConfig& profile) {
            return profile.settings.enabled;
        });

    if (!main_g_PseudoOverlay && !main_g_Config.pseudoOverlay.enabled && !anyProfileEnabled)
        return;

    if (!main_g_PseudoOverlay) {
        LogInfo("[Controller] Initializing pseudo-overlay (%s)...", reason ? reason : "configuration");
        auto overlay = std::make_unique<PseudoOverlay>();
        overlay->UpdateConfig(main_g_Config.pseudoOverlay, profiles);
        overlay->SetRecordingStartIntent(main_g_RecordingStartIntent.load(std::memory_order_acquire));
        HMODULE hMod = GetModuleHandle(NULL);
        if (!overlay->Init(reinterpret_cast<HINSTANCE>(hMod))) {
            LogError("[Controller] Failed to initialize pseudo-overlay");
            return;
        }
        main_g_PseudoOverlay = std::move(overlay);
        LogInfo("[Controller] Pseudo-overlay initialized");
        return;
    }

    main_g_PseudoOverlay->UpdateConfig(main_g_Config.pseudoOverlay, profiles);
}
