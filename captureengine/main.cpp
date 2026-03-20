// CaptureEngine Multi-Process Architecture
// Main entry point - acts as Controller when run without --mode flag
// Dispatches to Inject, Media, or Limiter process based on --mode=<mode>

// clang-format off
#include <windows.h>
#include <shellapi.h>
#include <timeapi.h>
#include <winreg.h>
// clang-format on
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>
#include "../common/config.h"
#include "../common/crash_handler.h"
#include "../common/logging.h"
#include "../common/process_ipc.h"
#include "../common/shared_defs.h"
#include "injection.h"
#include "pseudo_overlay.h"
#include "tray.h"

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

// Forward declarations for process entry points
extern int InjectProcessMain(const AppConfig& config);
extern int MediaProcessMain(const AppConfig& config);
extern int LimiterProcessMain(const AppConfig& config);
extern int LoggerProcessMain(const AppConfig& config);
extern int SensorProcessMain(const AppConfig& config);

// Hotkey IDs
#define HOTKEY_ID_RECORD 1

// Controller state
static bool g_Running = true;
static bool g_Recording = false;
static AppConfig g_Config;
static std::string g_ConfigPath;

// Auto-record feature for autonomous testing
static bool g_AutoRecordEnabled = false;
static DWORD g_AutoRecordDelayMs = 3000;      // Delay before starting
static DWORD g_AutoRecordDurationMs = 10000;  // Recording duration
static DWORD g_AutoRecordStartTime = 0;

// Deferred game launch (for --launch mode)
static std::string g_DeferredLaunchPath;

// Child process handles
static HANDLE g_hInjectProcess = NULL;
static HANDLE g_hMediaProcess = NULL;
static HANDLE g_hLimiterProcess = NULL;
static HANDLE g_hLoggerProcess = NULL;
static HANDLE g_hSensorProcess = NULL;

// IPC clients for child processes
static std::unique_ptr<ProcessIPCClient> g_InjectClient;
static std::unique_ptr<ProcessIPCClient> g_MediaClient;
static std::unique_ptr<ProcessIPCClient> g_LimiterClient;

static TrayIcon* g_Tray = nullptr;
static std::unique_ptr<PseudoOverlay> g_PseudoOverlay;

namespace {
constexpr UINT kMsgCompleteControllerStartup = WM_APP + 1;

struct ControllerStartupTimingState {
    int64_t controllerStartUs = 0;
    int64_t vulkanRegUs = 0;
    int64_t trayCreateUs = 0;
    bool complete = false;
};

ControllerStartupTimingState g_ControllerStartupTiming;

double QpcDeltaToMs(int64_t deltaUs) {
    return static_cast<double>(deltaUs) / 1000.0;
}

void PumpStartupMessages() {
    MSG msg = {};
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

void PrimeStartupCursor() {
    MSG msg = {};
    PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE);
    HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW);
    if (arrow) {
        SetCursor(arrow);
    }
}

bool IsProcessRunning(HANDLE hProcess) {
    if (!hProcess) {
        return false;
    }
    DWORD exitCode = 0;
    return GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE;
}

bool ShouldStartMediaProcessAtStartup() {
    return g_AutoRecordEnabled;
}

bool ShouldStartLimiterProcessAtStartup(const AppConfig& config) {
    return config.fpsLimiter.generalEnabled || (g_AutoRecordEnabled && config.fpsLimiter.captureSyncEnabled);
}

bool ShouldKeepLimiterProcessRunning(const AppConfig& config) {
    return config.fpsLimiter.generalEnabled || (g_Recording && config.fpsLimiter.captureSyncEnabled) ||
           (g_AutoRecordEnabled && config.fpsLimiter.captureSyncEnabled);
}

bool ShouldStartLoggerProcess(const AppConfig& config) {
    return config.debugLogging;
}

bool ShouldStartSensorProcess(const AppConfig& config) {
    return config.overlay.showCPU || config.overlay.showGPU || config.overlay.showRAM || config.overlay.showVRAM;
}

DWORD GetControllerLoopWaitMs(DWORD lastConfigCheck) {
    DWORD waitMs = 1000;
    DWORD now = GetTickCount();

    DWORD configElapsed = now - lastConfigCheck;
    if (configElapsed >= 2000) {
        return 0;
    }
    DWORD configWaitMs = 2000 - configElapsed;
    if (configWaitMs < waitMs) {
        waitMs = configWaitMs;
    }

    if (g_AutoRecordEnabled && g_AutoRecordStartTime > 0) {
        DWORD elapsed = now - g_AutoRecordStartTime;
        DWORD nextAutoActionMs = !g_Recording ? g_AutoRecordDelayMs : (g_AutoRecordDelayMs + g_AutoRecordDurationMs);
        if (elapsed >= nextAutoActionMs) {
            return 0;
        }
        DWORD autoWaitMs = nextAutoActionMs - elapsed;
        if (autoWaitMs < waitMs) {
            waitMs = autoWaitMs;
        }
    }

    return waitMs;
}

bool HotkeyConfigEquals(const AppConfig::HotkeyConfig& a, const AppConfig::HotkeyConfig& b) {
    return a.vkey == b.vkey && a.ctrl == b.ctrl && a.shift == b.shift && a.alt == b.alt && a.win == b.win;
}

void CloseProcessHandle(HANDLE& processHandle) {
    if (processHandle) {
        CloseHandle(processHandle);
        processHandle = NULL;
    }
}

bool EnsureChildProcessConnected(ProcessMode mode, HANDLE& processHandle, ProcessIPCClient* client, DWORD timeoutMs,
                                 const char* processName) {
    if (!processHandle || !IsProcessRunning(processHandle)) {
        if (client) {
            client->Disconnect();
        }
        if (processHandle) {
            CloseHandle(processHandle);
            processHandle = NULL;
        }

        const int64_t spawnStartUs = Log_GetQpcUs();
        processHandle = SpawnChildProcess(mode, g_ConfigPath.c_str());
        const int64_t spawnUs = Log_GetQpcUs() - spawnStartUs;
        if (!processHandle) {
            LogError("[Controller] Failed to spawn %s process on demand", processName);
            return false;
        }
        LogInfo("[Controller] Spawned %s process on demand in %.3f ms", processName, QpcDeltaToMs(spawnUs));
    }

    if (!client || client->IsConnected()) {
        return true;
    }

    const DWORD startTime = GetTickCount();
    while (g_Running && GetTickCount() - startTime < timeoutMs) {
        if (client->Connect(10)) {
            return true;
        }
        if (!IsProcessRunning(processHandle)) {
            break;
        }
        PumpStartupMessages();
        PrimeStartupCursor();
        MsgWaitForMultipleObjectsEx(0, nullptr, 25, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    LogError("[Controller] Failed to connect to %s process within %lu ms", processName,
             static_cast<unsigned long>(timeoutMs));
    return false;
}

bool EnsureMediaProcessReady(DWORD timeoutMs) {
    return EnsureChildProcessConnected(ProcessMode::Media, g_hMediaProcess, g_MediaClient.get(), timeoutMs, "media");
}

bool EnsureLimiterProcessReady(DWORD timeoutMs) {
    return EnsureChildProcessConnected(ProcessMode::Limiter, g_hLimiterProcess, g_LimiterClient.get(), timeoutMs,
                                       "limiter");
}

bool ShutdownIpcChildProcess(HANDLE& processHandle, ProcessIPCClient* client, const char* processName,
                             DWORD timeoutMs) {
    if (!processHandle) {
        if (client) {
            client->Disconnect();
        }
        return true;
    }

    if (client && client->IsConnected()) {
        ProcessResponse response = ProcessResponse::Ack;
        if (!client->SendCommand(ProcessCommand::Shutdown, nullptr, &response, timeoutMs)) {
            LogWarn("[Controller] Failed to send shutdown command to %s process", processName);
        }
        client->Disconnect();
    }

    DWORD waitResult = WaitForSingleObject(processHandle, timeoutMs);
    if (waitResult != WAIT_OBJECT_0) {
        LogWarn("[Controller] Timed out waiting for %s process to exit", processName);
    }
    CloseProcessHandle(processHandle);
    return waitResult == WAIT_OBJECT_0;
}

void SyncLimiterProcess(const AppConfig& config) {
    if (ShouldKeepLimiterProcessRunning(config)) {
        EnsureLimiterProcessReady(10000);
        return;
    }

    if (g_hLimiterProcess) {
        LogInfo("[Controller] Limiter no longer needed; shutting it down");
        ShutdownIpcChildProcess(g_hLimiterProcess, g_LimiterClient.get(), "limiter", 5000);
    } else if (g_LimiterClient) {
        g_LimiterClient->Disconnect();
    }
}

void SyncLoggerAndSensorProcesses(const AppConfig& config) {
    const bool wantLogger = ShouldStartLoggerProcess(config);
    const bool wantSensor = ShouldStartSensorProcess(config);
    const bool loggerRunning = IsProcessRunning(g_hLoggerProcess);
    const bool sensorRunning = IsProcessRunning(g_hSensorProcess);

    if (loggerRunning == wantLogger && sensorRunning == wantSensor) {
        if (!loggerRunning) {
            CloseProcessHandle(g_hLoggerProcess);
        }
        if (!sensorRunning) {
            CloseProcessHandle(g_hSensorProcess);
        }
        return;
    }

    LogInfo("[Controller] Reloading logger/sensor services (logger=%d sensor=%d)", wantLogger ? 1 : 0,
            wantSensor ? 1 : 0);

    wchar_t shutdownEventName[64];
    GenerateShutdownEventName(shutdownEventName, 64, GetCurrentProcessId());
    HANDLE hShutdownEvent = CreateEventW(NULL, TRUE, FALSE, shutdownEventName);
    if (hShutdownEvent) {
        SetEvent(hShutdownEvent);
    }

    if (g_hLoggerProcess) {
        WaitForSingleObject(g_hLoggerProcess, 5000);
        CloseProcessHandle(g_hLoggerProcess);
    }
    if (g_hSensorProcess) {
        WaitForSingleObject(g_hSensorProcess, 5000);
        CloseProcessHandle(g_hSensorProcess);
    }

    if (hShutdownEvent) {
        ResetEvent(hShutdownEvent);
        CloseHandle(hShutdownEvent);
    }

    if (wantLogger) {
        g_hLoggerProcess = SpawnChildProcess(ProcessMode::Logger, g_ConfigPath.c_str());
        if (!g_hLoggerProcess) {
            LogError("[Controller] Failed to restart logger process");
        }
    }
    if (wantSensor) {
        g_hSensorProcess = SpawnChildProcess(ProcessMode::Sensors, g_ConfigPath.c_str());
        if (!g_hSensorProcess) {
            LogError("[Controller] Failed to restart sensor process");
        }
    }
}
}  // namespace

// Remove old session directories from logs/, keeping the most recent maxKeep.
// Also cleans up any stale flat .log/.csv files from pre-session-dir versions.
static void CleanupOldSessionDirs(const std::string& logsDir, size_t maxKeep = 20) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // Collect session subdirectories (names are YYYYMMDD_HHMMSS, so lexicographic sort = chronological)
    std::vector<fs::directory_entry> sessions;
    for (auto& entry : fs::directory_iterator(logsDir, ec)) {
        if (!entry.is_directory(ec))
            continue;
        auto name = entry.path().filename().string();
        // Validate timestamp format: 15 chars, YYYYMMDD_HHMMSS
        if (name.size() == 15 && name[8] == '_')
            sessions.push_back(entry);
    }

    // Sort oldest-first by name
    std::sort(sessions.begin(), sessions.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.path().filename() < b.path().filename();
    });

    // Remove oldest sessions beyond maxKeep
    if (sessions.size() > maxKeep) {
        size_t toRemove = sessions.size() - maxKeep;
        for (size_t i = 0; i < toRemove; i++) {
            fs::remove_all(sessions[i].path(), ec);
        }
    }

    // Clean up stale flat files from pre-session-dir versions
    const char* patterns[] = {"\\*.log", "\\*.csv"};
    for (int p = 0; p < 2; p++) {
        std::string pattern = logsDir + patterns[p];
        WIN32_FIND_DATAA ffd;
        HANDLE hFind = FindFirstFileA(pattern.c_str(), &ffd);
        if (hFind == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            std::string filePath = logsDir + "\\" + ffd.cFileName;
            DeleteFileA(filePath.c_str());
        } while (FindNextFileA(hFind, &ffd));
        FindClose(hFind);
    }
}

// Launch game suspended and inject immediately (The only way to guarantee API
// overrides) If the target looks like a launcher (not the actual game exe), we
// just start it normally and let WMI + CreateProcess hooks in already-injected
// processes catch the real game
void LaunchGameSuspended(const std::string& path) {
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi = {0};

    // Strip surrounding quotes if present
    std::string cleanPath = path;
    if (cleanPath.length() >= 2 && cleanPath.front() == '"' && cleanPath.back() == '"') {
        cleanPath = cleanPath.substr(1, cleanPath.length() - 2);
    }

    // Extract filename
    size_t lastSlash = cleanPath.find_last_of("\\/");
    std::string filename = (lastSlash != std::string::npos) ? cleanPath.substr(lastSlash + 1) : cleanPath;

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

    // Extract directory
    std::string dir = cleanPath.substr(0, cleanPath.find_last_of("\\/"));

    if (looksLikeLauncher) {
        // This looks like a launcher - start it NORMALLY, no injection
        // We'll rely on WMI to catch the actual game
        LogInfo("[Launcher] Detected launcher (not game): %s - Starting normally", cleanPath.c_str());

        if (CreateProcessA(cleanPath.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, dir.c_str(), &si, &pi)) {
            LogInfo("[Launcher] Launcher started (PID: %d). WMI will catch the game.", pi.dwProcessId);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            LogError("[Launcher] Failed to start launcher: %d", GetLastError());
        }
    } else {
        // This looks like the actual game - use suspended injection
        LogInfo("[Launcher] Detected game: %s - Launching Suspended", cleanPath.c_str());

        if (CreateProcessA(cleanPath.c_str(), NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, dir.c_str(), &si, &pi)) {
            LogInfo(
                "[Launcher] Process Created (PID: %d). Attempting early APC "
                "injection...",
                pi.dwProcessId);

            auto injector = std::make_shared<InjectionManager>(g_Config);

            // Determine DLL path based on target architecture
            BOOL isWow64 = FALSE;
            HANDLE hCheckProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pi.dwProcessId);
            if (hCheckProcess) {
                IsWow64Process(hCheckProcess, &isWow64);
                CloseHandle(hCheckProcess);
            }

            std::string hookDllPath;
            char buffer[MAX_PATH];
            GetModuleFileNameA(NULL, buffer, MAX_PATH);
            std::string baseDir = std::string(buffer).substr(0, std::string(buffer).find_last_of("\\/"));
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
                if (injector->Inject(pi.dwProcessId, cleanPath)) {
                    LogInfo("[Launcher] Fallback injection successful.");
                } else {
                    LogError(
                        "[Launcher] Fallback injection FAILED. Game running without "
                        "hooks.");
                }
            }

            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            LogError("[Launcher] Failed to CreateProcess: %d", GetLastError());
        }
    }
}

// Connect to child processes with retry
bool ConnectToChildProcesses(DWORD timeoutMs) {
    DWORD startTime = GetTickCount();

    while (g_Running && GetTickCount() - startTime < timeoutMs) {
        bool allConnected = true;

        if (g_hInjectProcess && !g_InjectClient->IsConnected()) {
            if (!g_InjectClient->Connect(10))
                allConnected = false;
        }
        if (g_hMediaProcess && !g_MediaClient->IsConnected()) {
            if (!g_MediaClient->Connect(10))
                allConnected = false;
        }
        if (g_hLimiterProcess && !g_LimiterClient->IsConnected()) {
            if (!g_LimiterClient->Connect(10))
                allConnected = false;
        }
        // Note: Logger and Sensors don't use pipe IPC yet, they use shared
        // memory/files

        if (allConnected)
            return true;
        PumpStartupMessages();
        PrimeStartupCursor();
        MsgWaitForMultipleObjectsEx(0, nullptr, 25, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        PumpStartupMessages();
    }

    return false;
}

// Send command to all child processes
void SendCommandToAll(ProcessCommand cmd) {
    if (g_InjectClient && g_InjectClient->IsConnected()) {
        g_InjectClient->SendCommand(cmd);
    }
    if (g_MediaClient && g_MediaClient->IsConnected()) {
        g_MediaClient->SendCommand(cmd);
    }
    if (g_LimiterClient && g_LimiterClient->IsConnected()) {
        g_LimiterClient->SendCommand(cmd);
    }
}

// Toggle recording - controller notifies inject which sets shared memory
// Media process polls shared memory flags - more reliable than pipe IPC
void ToggleRecording() {
    g_Recording = !g_Recording;

    if (g_Recording) {
        LogInfo("[Controller] Starting recording...");

        if (!EnsureMediaProcessReady(10000)) {
            LogError("[Controller] Media process is not ready, cannot start recording");
            g_Recording = false;
            if (g_Tray)
                g_Tray->SetRecordingState(false);
            return;
        }

        if (g_Config.fpsLimiter.captureSyncEnabled && !EnsureLimiterProcessReady(10000)) {
            LogError("[Controller] Limiter process is not ready for capture-synced recording");
            g_Recording = false;
            if (g_Tray)
                g_Tray->SetRecordingState(false);
            return;
        }

        // Notify inject process - it sets shared memory flags and media polls them
        if (g_InjectClient && g_InjectClient->IsConnected()) {
            ProcessResponse resp;
            if (g_InjectClient->SendCommand(ProcessCommand::StartRecording, nullptr, &resp, 5000)) {
                LogInfo("[Controller] Recording started");
            } else {
                LogError("[Controller] Failed to notify inject process - retrying once");
                // Retry once on failure
                if (g_InjectClient->SendCommand(ProcessCommand::StartRecording, nullptr, &resp, 5000)) {
                    LogInfo("[Controller] Recording started (retry)");
                } else {
                    LogError("[Controller] Recording start failed");
                    g_Recording = false;
                }
            }
        }
    } else {
        LogInfo("[Controller] Stopping recording...");

        // Notify inject process - it clears shared memory recording flag
        if (g_InjectClient && g_InjectClient->IsConnected()) {
            ProcessResponse resp;
            if (!g_InjectClient->SendCommand(ProcessCommand::StopRecording, nullptr, &resp, 5000)) {
                LogError("[Controller] Stop failed - retrying once");
                g_InjectClient->SendCommand(ProcessCommand::StopRecording, nullptr, &resp, 5000);
            }
        }

        // Media process self-exits after recording stops to free GPU VRAM.
        // Pre-clear handle so EnsureChildProcessConnected spawns fresh next time.
        if (g_MediaClient)
            g_MediaClient->Disconnect();
        if (g_hMediaProcess) {
            CloseHandle(g_hMediaProcess);
            g_hMediaProcess = NULL;
        }

        LogInfo("[Controller] Recording stopped");
    }

    if (g_Tray)
        g_Tray->SetRecordingState(g_Recording);

    if (g_PseudoOverlay)
        g_PseudoOverlay->SetRecordingState(g_Recording);
}

// Shutdown all child processes gracefully
void ShutdownChildProcesses() {
    LogInfo("[Controller] Shutting down child processes...");

    // Signal Logger and Sensor processes to exit via named event
    wchar_t shutdownEventName[64];
    GenerateShutdownEventName(shutdownEventName, 64, GetCurrentProcessId());
    HANDLE hShutdownEvent = CreateEventW(NULL, TRUE, FALSE, shutdownEventName);
    if (hShutdownEvent) {
        SetEvent(hShutdownEvent);
        CloseHandle(hShutdownEvent);
    }

    // Send shutdown commands
    SendCommandToAll(ProcessCommand::Shutdown);

    // Wait for processes to exit
    HANDLE handles[5];  // Increased size for Logger and Sensor
    const char* handleNames[5] = {};
    int handleCount = 0;

    if (g_hLimiterProcess) {
        handles[handleCount] = g_hLimiterProcess;
        handleNames[handleCount++] = "Limiter";
    }
    if (g_hMediaProcess) {
        handles[handleCount] = g_hMediaProcess;
        handleNames[handleCount++] = "Media";
    }
    if (g_hInjectProcess) {
        handles[handleCount] = g_hInjectProcess;
        handleNames[handleCount++] = "Inject";
    }
    if (g_hLoggerProcess) {
        handles[handleCount] = g_hLoggerProcess;
        handleNames[handleCount++] = "Logger";
    }
    if (g_hSensorProcess) {
        handles[handleCount] = g_hSensorProcess;
        handleNames[handleCount++] = "Sensor";
    }

    if (handleCount > 0) {
        // Use MsgWaitForMultipleObjects to keep processing messages (for tray
        // animation)
        DWORD startTime = GetTickCount();
        // INCREASED TIMEOUT: Media process needs more time to flush video/audio
        // data especially for high-resolution recordings (4K 120fps)
        DWORD timeout = 10000;  // 10 seconds (was 5)
        bool allExited = false;

        while (!allExited && (GetTickCount() - startTime) < timeout) {
            DWORD remaining = timeout - (GetTickCount() - startTime);
            DWORD waitTime = (remaining < 100) ? remaining : 100;

            // bWaitAll MUST be FALSE to process messages while waiting
            DWORD result = MsgWaitForMultipleObjects(handleCount, handles, FALSE, waitTime, QS_ALLINPUT);

            if (result == WAIT_OBJECT_0 + handleCount) {
                // Messages available - process them to keep tray animation running
                MSG msg;
                while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
            } else if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + handleCount) {
                // At least one process exited, re-evaluate all processes
                bool foundActive = false;
                for (int i = 0; i < handleCount; i++) {
                    DWORD exitCode;
                    if (GetExitCodeProcess(handles[i], &exitCode) && exitCode == STILL_ACTIVE) {
                        foundActive = true;
                        break;
                    }
                }
                if (!foundActive)
                    allExited = true;
            } else {
                // Timeout or other error
            }
        }

        if (!allExited) {
            // Log which specific processes didn't exit cleanly for debugging
            LogInfo(
                "[Controller] Some processes didn't exit cleanly within timeout, "
                "terminating...");
            for (int i = 0; i < handleCount; i++) {
                DWORD exitCode;
                if (GetExitCodeProcess(handles[i], &exitCode) && exitCode == STILL_ACTIVE) {
                    LogInfo(
                        "[Controller] %s process did not exit cleanly, forcing "
                        "termination",
                        handleNames[i]);
                }
            }
            if (g_hLimiterProcess)
                TerminateProcess(g_hLimiterProcess, 1);
            if (g_hMediaProcess)
                TerminateProcess(g_hMediaProcess, 1);
            if (g_hInjectProcess)
                TerminateProcess(g_hInjectProcess, 1);
            if (g_hLoggerProcess)
                TerminateProcess(g_hLoggerProcess, 1);
            if (g_hSensorProcess)
                TerminateProcess(g_hSensorProcess, 1);
        } else {
            LogInfo("[Controller] All child processes exited cleanly");
        }
    }

    // Cleanup handles
    if (g_hLimiterProcess)
        CloseHandle(g_hLimiterProcess);
    if (g_hMediaProcess)
        CloseHandle(g_hMediaProcess);
    if (g_hInjectProcess)
        CloseHandle(g_hInjectProcess);
    if (g_hLoggerProcess)
        CloseHandle(g_hLoggerProcess);
    if (g_hSensorProcess)
        CloseHandle(g_hSensorProcess);

    g_hLimiterProcess = NULL;
    g_hMediaProcess = NULL;
    g_hInjectProcess = NULL;
    g_hLoggerProcess = NULL;
    g_hSensorProcess = NULL;
}

// Monitor child processes for crashes
void CheckChildProcessHealth() {
    static DWORD lastCheck = 0;
    if (GetTickCount() - lastCheck < 1000)
        return;  // Check once per second
    lastCheck = GetTickCount();

    auto checkProcess = [](HANDLE h, const char* name, bool& reportedDead) {
        if (!h)
            return;
        DWORD exitCode;
        if (GetExitCodeProcess(h, &exitCode) && exitCode != STILL_ACTIVE) {
            if (!reportedDead) {
                LogError("[Controller] %s process exited with code %d", name, exitCode);
                reportedDead = true;
            }
        } else {
            reportedDead = false;  // Reset in case the process was restarted
        }
    };

    static bool injectDead = false, mediaDead = false, limiterDead = false;
    checkProcess(g_hInjectProcess, "Inject", injectDead);
    checkProcess(g_hMediaProcess, "Media", mediaDead);
    checkProcess(g_hLimiterProcess, "Limiter", limiterDead);
}

bool CompleteControllerStartup() {
    if (g_ControllerStartupTiming.complete) {
        return true;
    }

    LogInfo("[Controller] Completing deferred startup...");
    LogInfo("[Controller] Spawning child processes...");

    const int64_t injectSpawnStartUs = Log_GetQpcUs();
    g_hInjectProcess = SpawnChildProcess(ProcessMode::Inject, g_ConfigPath.c_str());
    const int64_t injectSpawnUs = Log_GetQpcUs() - injectSpawnStartUs;
    if (!g_hInjectProcess) {
        LogError("[Controller] Failed to spawn inject process");
        return false;
    }

    int64_t mediaSpawnUs = 0;
    if (ShouldStartMediaProcessAtStartup()) {
        const int64_t mediaSpawnStartUs = Log_GetQpcUs();
        g_hMediaProcess = SpawnChildProcess(ProcessMode::Media, g_ConfigPath.c_str());
        mediaSpawnUs = Log_GetQpcUs() - mediaSpawnStartUs;
        if (!g_hMediaProcess) {
            LogError("[Controller] Failed to spawn media process");
            return false;
        }
    } else {
        LogInfo("[Controller] Deferring media process startup until recording begins");
    }

    int64_t limiterSpawnUs = 0;
    if (ShouldStartLimiterProcessAtStartup(g_Config)) {
        const int64_t limiterSpawnStartUs = Log_GetQpcUs();
        g_hLimiterProcess = SpawnChildProcess(ProcessMode::Limiter, g_ConfigPath.c_str());
        limiterSpawnUs = Log_GetQpcUs() - limiterSpawnStartUs;
        if (!g_hLimiterProcess) {
            LogError("[Controller] Failed to spawn limiter process");
            return false;
        }
    } else {
        LogInfo("[Controller] Deferring limiter process startup until a limiter is enabled");
    }

    const int64_t auxSpawnStartUs = Log_GetQpcUs();
    if (ShouldStartLoggerProcess(g_Config)) {
        g_hLoggerProcess = SpawnChildProcess(ProcessMode::Logger, g_ConfigPath.c_str());
        if (!g_hLoggerProcess) {
            LogError("[Controller] Failed to spawn logger process");
        }
    }
    if (ShouldStartSensorProcess(g_Config)) {
        g_hSensorProcess = SpawnChildProcess(ProcessMode::Sensors, g_ConfigPath.c_str());
        if (!g_hSensorProcess) {
            LogError("[Controller] Failed to spawn sensor process");
        }
    }
    const int64_t auxSpawnUs = Log_GetQpcUs() - auxSpawnStartUs;

    LogInfo("[Controller] Waiting for child processes to connect...");
    const int64_t ipcConnectStartUs = Log_GetQpcUs();
    if (!ConnectToChildProcesses(10000)) {
        LogError("[Controller] Failed to connect to all child processes");
        return false;
    }
    const int64_t ipcConnectUs = Log_GetQpcUs() - ipcConnectStartUs;
    LogInfo("[Controller] All child processes connected");

    if (!g_DeferredLaunchPath.empty()) {
        LogInfo("[Controller] Launching deferred game: %s", g_DeferredLaunchPath.c_str());
        LaunchGameSuspended(g_DeferredLaunchPath);
    }

    LogInfo("[Controller] Registering hotkeys...");
    const int64_t hotkeyStartUs = Log_GetQpcUs();
    RegisterHotKey(NULL, HOTKEY_ID_RECORD, g_Config.hotkeyStartStop.GetModifiers(), g_Config.hotkeyStartStop.vkey);
    const int64_t hotkeyUs = Log_GetQpcUs() - hotkeyStartUs;

    // Initialize pseudo-overlay for WGC capture
    if (g_Config.pseudoOverlay.enabled) {
        LogInfo("[Controller] Initializing pseudo-overlay...");
        g_PseudoOverlay = std::make_unique<PseudoOverlay>();
        HMODULE hMod = GetModuleHandle(NULL);
        if (g_PseudoOverlay->Init(reinterpret_cast<HINSTANCE>(hMod))) {
            g_PseudoOverlay->UpdateConfig(g_Config.pseudoOverlay);
            g_PseudoOverlay->SetRecordingState(g_Recording);
            LogInfo("[Controller] Pseudo-overlay initialized");
        } else {
            LogError("[Controller] Failed to initialize pseudo-overlay");
            g_PseudoOverlay.reset();
        }
    }

    LogInfo("[Controller] Ready. Press hotkey to start recording.");
    PrimeStartupCursor();
    LogInfo(
        "[StartupPerf] Controller startup: VulkanRegistration=%.3f ms, SpawnInject=%.3f ms, "
        "SpawnMedia=%.3f ms, SpawnLimiter=%.3f ms, SpawnAux=%.3f ms, IPCConnect=%.3f ms, TrayCreate=%.3f ms, "
        "RegisterHotkeys=%.3f ms, TotalToReady=%.3f ms",
        QpcDeltaToMs(g_ControllerStartupTiming.vulkanRegUs), QpcDeltaToMs(injectSpawnUs), QpcDeltaToMs(mediaSpawnUs),
        QpcDeltaToMs(limiterSpawnUs), QpcDeltaToMs(auxSpawnUs), QpcDeltaToMs(ipcConnectUs),
        QpcDeltaToMs(g_ControllerStartupTiming.trayCreateUs), QpcDeltaToMs(hotkeyUs),
        QpcDeltaToMs(Log_GetQpcUs() - g_ControllerStartupTiming.controllerStartUs));

    if (g_AutoRecordEnabled) {
        LogInfo("[Controller] Auto-record enabled: delay=%dms, duration=%dms", g_AutoRecordDelayMs,
                g_AutoRecordDurationMs);
        g_AutoRecordStartTime = GetTickCount();
    }

    g_ControllerStartupTiming.complete = true;
    return true;
}

static void Registry_ManageImplicitLayer(bool install) {
    // IMPORTANT: HKEY_CURRENT_USER does NOT get WOW64 redirected!
    // Both 32-bit and 64-bit apps read from the same HKCU\Software\Khronos\Vulkan\ImplicitLayers path.
    // We must register BOTH manifests there so apps of either architecture can find their layer.
    // Each manifest has a unique name (VK_LAYER_CE_overlay vs VK_LAYER_CE_overlay_x86)
    // to avoid "wrong bit-type" conflicts.

    HKEY hKey;
    const char* regPath = "Software\\Khronos\\Vulkan\\ImplicitLayers";

    LogInfo("[Controller] Attempting to %s registry key: %s", install ? "create/open" : "open", regPath);

    LONG result =
        RegCreateKeyExA(HKEY_CURRENT_USER, regPath, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (result == ERROR_SUCCESS) {
        LogInfo("[Controller] Successfully opened registry key: %s", regPath);

        char buffer[MAX_PATH];
        GetModuleFileNameA(NULL, buffer, MAX_PATH);
        std::string baseDir = buffer;
        baseDir = baseDir.substr(0, baseDir.find_last_of("\\/"));

        // Register BOTH manifests - each has unique name and correct library_path
        const char* manifests[] = {"VK_LAYER_CE_overlay.json", "VK_LAYER_CE_overlay_x86.json"};

        for (const char* manifest : manifests) {
            std::string fullPath = baseDir + "\\" + manifest;
            if (install) {
                DWORD data = 0;
                LONG setResult = RegSetValueExA(hKey, fullPath.c_str(), 0, REG_DWORD, (const BYTE*)&data, sizeof(data));
                if (setResult == ERROR_SUCCESS) {
                    LogInfo("[Controller] Registered Vulkan Layer: %s", manifest);
                } else {
                    LogError("[Controller] Failed to register Vulkan Layer: %s, error=%ld", manifest, setResult);
                }
            } else {
                LONG delResult = RegDeleteValueA(hKey, fullPath.c_str());
                if (delResult == ERROR_SUCCESS || delResult == ERROR_FILE_NOT_FOUND) {
                    LogInfo("[Controller] Unregistered Vulkan Layer: %s", manifest);
                } else {
                    LogError("[Controller] Failed to unregister Vulkan Layer: %s, error=%ld", manifest, delResult);
                }
            }
        }
        RegCloseKey(hKey);
    } else {
        LogError("[Controller] Failed to open registry key for Vulkan Layer, error=%ld", result);
    }
}

// RAII Wrapper for guaranteed cleanup
class ScopedVulkanRegistration {
public:
    ScopedVulkanRegistration() {
        Registry_ManageImplicitLayer(true);
    }
    ~ScopedVulkanRegistration() {
        Registry_ManageImplicitLayer(false);
    }
};

// Global pointer for emergency cleanup
static ScopedVulkanRegistration* g_VulkanReg = nullptr;

BOOL WINAPI ControllerConsoleHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT ||
        ctrlType == CTRL_LOGOFF_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
        LogInfo("[Controller] Console event %lu received. Cleaning up...", ctrlType);
        if (g_VulkanReg) {
            Registry_ManageImplicitLayer(false);  // Force cleanup
        }
        g_Running = false;
        return TRUE;
    }
    return FALSE;
}

// Controller main function
int ControllerMain(HINSTANCE hInstance) {
    const int64_t controllerStartUs = Log_GetQpcUs();
    LogInfo("[Controller] Starting...");
    PrimeStartupCursor();

    SetConsoleCtrlHandler(ControllerConsoleHandler, TRUE);

    // Ephemeral Registration (RAII)
    const int64_t vulkanRegStartUs = Log_GetQpcUs();
    ScopedVulkanRegistration vulkanReg;
    const int64_t vulkanRegUs = Log_GetQpcUs() - vulkanRegStartUs;
    g_VulkanReg = &vulkanReg;

    // Create IPC clients
    g_InjectClient = std::make_unique<ProcessIPCClient>(ProcessMode::Inject);
    g_MediaClient = std::make_unique<ProcessIPCClient>(ProcessMode::Media);
    g_LimiterClient = std::make_unique<ProcessIPCClient>(ProcessMode::Limiter);

    // Create tray icon early so Explorer can clear the launch wait cursor even
    // while child processes are still spinning up.
    LogInfo("[Controller] Creating tray icon...");
    const int64_t trayCreateStartUs = Log_GetQpcUs();
    auto tray = std::make_unique<TrayIcon>(
        hInstance, []() { g_Running = false; },
        []() { ShellExecuteA(NULL, "open", g_ConfigPath.c_str(), NULL, NULL, SW_SHOW); });
    const int64_t trayCreateUs = Log_GetQpcUs() - trayCreateStartUs;
    g_Tray = tray.get();
    PrimeStartupCursor();
    PumpStartupMessages();

    g_ControllerStartupTiming.controllerStartUs = controllerStartUs;
    g_ControllerStartupTiming.vulkanRegUs = vulkanRegUs;
    g_ControllerStartupTiming.trayCreateUs = trayCreateUs;
    g_ControllerStartupTiming.complete = false;
    PostThreadMessage(GetCurrentThreadId(), kMsgCompleteControllerStartup, 0, 0);

    // Main message loop
    MSG msg;
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    while (g_Running) {
        // Process messages
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_Running = false;
                continue;
            }
            if (msg.message == kMsgCompleteControllerStartup) {
                if (!CompleteControllerStartup()) {
                    ShutdownChildProcesses();
                    g_Running = false;
                }
                continue;
            }
            if (msg.message == WM_HOTKEY) {
                if (msg.wParam == HOTKEY_ID_RECORD) {
                    ToggleRecording();
                }
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // Check child process health
        CheckChildProcessHealth();

        // Config hot-reload
        static DWORD lastConfigCheck = 0;
        if (GetTickCount() - lastConfigCheck > 2000) {
            WIN32_FILE_ATTRIBUTE_DATA fileInfo;
            if (GetFileAttributesExA(g_ConfigPath.c_str(), GetFileExInfoStandard, &fileInfo)) {
                static FILETIME lastWriteTime = fileInfo.ftLastWriteTime;
                if (CompareFileTime(&fileInfo.ftLastWriteTime, &lastWriteTime) > 0) {
                    LogInfo("[Controller] Config change detected, reloading...");
                    lastWriteTime = fileInfo.ftLastWriteTime;

                    AppConfig oldConfig = g_Config;
                    LoadConfig(g_ConfigPath, g_Config);

                    if (!HotkeyConfigEquals(oldConfig.hotkeyStartStop, g_Config.hotkeyStartStop)) {
                        UnregisterHotKey(NULL, HOTKEY_ID_RECORD);
                        if (!RegisterHotKey(NULL, HOTKEY_ID_RECORD, g_Config.hotkeyStartStop.GetModifiers(),
                                            g_Config.hotkeyStartStop.vkey)) {
                            LogError("[Controller] Failed to re-register recording hotkey");
                        }
                    }

                    SyncLoggerAndSensorProcesses(g_Config);
                    SyncLimiterProcess(g_Config);
                    SendCommandToAll(ProcessCommand::ReloadConfig);

                    // Pseudo-overlay config hot-reload
                    if (g_PseudoOverlay) {
                        g_PseudoOverlay->UpdateConfig(g_Config.pseudoOverlay);
                    } else if (g_Config.pseudoOverlay.enabled) {
                        // Enable pseudo-overlay at runtime
                        g_PseudoOverlay = std::make_unique<PseudoOverlay>();
                        HMODULE hMod = GetModuleHandle(NULL);
                        if (g_PseudoOverlay->Init(reinterpret_cast<HINSTANCE>(hMod))) {
                            g_PseudoOverlay->UpdateConfig(g_Config.pseudoOverlay);
                            g_PseudoOverlay->SetRecordingState(g_Recording);
                            LogInfo("[Controller] Pseudo-overlay enabled via config hot-reload");
                        } else {
                            LogError("[Controller] Failed to init pseudo-overlay on hot-reload");
                            g_PseudoOverlay.reset();
                        }
                    }
                }
            }
            lastConfigCheck = GetTickCount();
        }

        // Auto-record logic
        if (g_AutoRecordEnabled && g_AutoRecordStartTime > 0) {
            DWORD elapsed = GetTickCount() - g_AutoRecordStartTime;
            if (!g_Recording && elapsed >= g_AutoRecordDelayMs) {
                LogInfo("[Controller] Auto-record: starting recording...");
                ToggleRecording();
            } else if (g_Recording && elapsed >= (g_AutoRecordDelayMs + g_AutoRecordDurationMs)) {
                LogInfo("[Controller] Auto-record: stopping recording...");
                ToggleRecording();
                g_Running = false;  // Exit after auto-record completes
            }
        }

        MsgWaitForMultipleObjectsEx(0, nullptr, GetControllerLoopWaitMs(lastConfigCheck), QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
    }

    // Unregister hotkeys first
    UnregisterHotKey(NULL, HOTKEY_ID_RECORD);

    // Keep tray icon alive during shutdown (animation already started by
    // right-click handler) Process messages during shutdown so animation
    // continues
    if (g_Tray) {
        g_Tray->StartShutdownAnimation();
    }

    // Shutdown pseudo-overlay before child processes
    if (g_PseudoOverlay) {
        g_PseudoOverlay->Shutdown();
        g_PseudoOverlay.reset();
    }

    ShutdownChildProcesses();

    // Reset global pointer (destructor of ScopedVulkanRegistration will handle
    // cleanup)
    g_VulkanReg = nullptr;

    // Now remove tray icon after shutdown is complete
    if (tray) {
        tray->Remove();
    }
    g_Tray = nullptr;
    tray.reset();

    LogInfo("[Controller] Exiting");
    return 0;
}

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Parse process mode from command line
    ProcessMode mode = ParseProcessMode(lpCmdLine);

    // Get paths
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::string exePath = buffer;
    std::string baseDir = exePath.substr(0, exePath.find_last_of("\\/"));
    g_ConfigPath = baseDir + "\\config.ini";

    // Install crash handler early (before config loading) so any startup crash
    // produces a minidump. The dump directory will be updated after config loads.
    std::string logsRootDir = baseDir + "\\logs";
    CreateDirectoryA(logsRootDir.c_str(), NULL);

    // Determine session directory: Controller generates a new timestamped folder,
    // child processes inherit the name from --session-dir= on the command line.
    if (mode == ProcessMode::Controller) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char ts[32];
        snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond);
        g_SessionDirName = ts;
        CleanupOldSessionDirs(logsRootDir);
    } else {
        g_SessionDirName = ParseSessionDir(lpCmdLine);
    }

    std::string earlyLogsDir;
    if (!g_SessionDirName.empty()) {
        earlyLogsDir = logsRootDir + "\\" + g_SessionDirName;
    } else {
        earlyLogsDir = logsRootDir;
    }
    CreateDirectoryA(earlyLogsDir.c_str(), NULL);
    SetCrashDumpDirectory(earlyLogsDir);
    InstallCrashHandler();

    // Load config
    LoadConfig(g_ConfigPath, g_Config);

    // Parse --auto-record flag: --auto-record=DELAY_MS,DURATION_MS
    // Parse --license flag
    std::string cmdLine(lpCmdLine);
    if (cmdLine.find("--license") != std::string::npos) {
        std::string licensePath = baseDir + "\\LICENSE.txt";
        ShellExecuteA(NULL, "open", licensePath.c_str(), NULL, NULL, SW_SHOWNORMAL);
        return 0;
    }
    size_t autoRecordPos = cmdLine.find("--auto-record=");
    if (autoRecordPos != std::string::npos) {
        std::string params = cmdLine.substr(autoRecordPos + 14);
        size_t commaPos = params.find(',');
        if (commaPos != std::string::npos) {
            g_AutoRecordDelayMs = (DWORD)atoi(params.substr(0, commaPos).c_str());
            g_AutoRecordDurationMs = (DWORD)atoi(params.substr(commaPos + 1).c_str());
            g_AutoRecordEnabled = true;
        }
    }

    // Parse --launch flag: --launch <command> or --launch=<command>
    // Supports Steam Launch Options: "CaptureEngine.exe" --launch %command%
    std::string searchFlag = "--launch";
    size_t launchPos = cmdLine.find(searchFlag);
    if (launchPos != std::string::npos) {
        size_t valueStart = launchPos + searchFlag.length();

        // Skip delimiter (+, =, or space)
        while (valueStart < cmdLine.length() && (cmdLine[valueStart] == '=' || cmdLine[valueStart] == ' ')) {
            valueStart++;
        }

        if (valueStart < cmdLine.length()) {
            g_DeferredLaunchPath = cmdLine.substr(valueStart);

            // Game launch will happen in ControllerMain AFTER child processes are
            // ready
            if (g_Config.debugLogging) {
                Log_Init(earlyLogsDir + "\\launcher.log");
                LogInfo("[Launcher] Deferred launch path: %s", g_DeferredLaunchPath.c_str());
            }

            // Continue as Controller
            mode = ProcessMode::Controller;
        }
    }

    // Setup logging with process-specific log file in session logs subfolder
    std::string logsDir = earlyLogsDir;
    CreateDirectoryA(logsDir.c_str(), NULL);
    std::string logPath = logsDir + "\\" + GetLogFileName(mode);
    g_Config.logFilePath = logPath;

    if (g_Config.debugLogging) {
        Log_Init(logPath);
        LogInfo("CaptureEngine Starting... Version: %s (Built: %s)", CAPTURE_VERSION, BUILD_TIMESTAMP);
        LogInfo("Process Mode: %s", mode == ProcessMode::Controller ? "Controller"
                                    : mode == ProcessMode::Inject   ? "Inject"
                                    : mode == ProcessMode::Media    ? "Media"
                                    : mode == ProcessMode::Limiter  ? "Limiter"
                                    : mode == ProcessMode::Logger   ? "Logger"
                                    : mode == ProcessMode::Sensors  ? "Sensors"
                                                                    : "Unknown");
    }

    PrimeStartupCursor();

    // Opt out of Windows 11 EcoQoS / power throttling for all sub-processes.
    // This tells the scheduler to prefer P-cores over E-cores on hybrid CPUs.
    PROCESS_POWER_THROTTLING_STATE pts = {};
    pts.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pts.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    pts.StateMask = 0;  // 0 = disable throttling (prefer performance cores)
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pts, sizeof(pts));

    // Controller: Single instance check
    if (mode == ProcessMode::Controller) {
        CreateMutexA(0, FALSE, "Local\\CaptureEngine_Instance_Mutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            MessageBoxA(NULL, "CaptureEngine is already running.", "Error", MB_ICONERROR);
            return 1;
        }
    }

    // Keep crash dumps under logs/. Config can only add a relative subfolder.
    std::string crashDir = logsDir;
    if (!g_Config.crashDumpDir.empty()) {
        std::filesystem::path configured = std::filesystem::path(g_Config.crashDumpDir).lexically_normal();
        bool hasParentTraversal = false;
        for (const auto& part : configured) {
            if (part == std::filesystem::path("..")) {
                hasParentTraversal = true;
                break;
            }
        }
        if (!configured.empty() && configured != "." && !configured.is_absolute() && !hasParentTraversal) {
            crashDir = (std::filesystem::path(logsDir) / configured).string();
        }
    }
    SetCrashDumpDirectory(crashDir);

    // Dispatch to appropriate process
    int result = 0;
    switch (mode) {
        case ProcessMode::Controller:
            result = ControllerMain(hInstance);
            break;
        case ProcessMode::Inject:
            result = InjectProcessMain(g_Config);
            break;
        case ProcessMode::Media:
            result = MediaProcessMain(g_Config);
            break;
        case ProcessMode::Limiter:
            result = LimiterProcessMain(g_Config);
            break;
        case ProcessMode::Logger:
            result = LoggerProcessMain(g_Config);
            break;
        case ProcessMode::Sensors:
            result = SensorProcessMain(g_Config);
            break;
    }

    // Cleanup
    Log_Shutdown();

    return result;
}
