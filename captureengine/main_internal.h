#pragma once

// clang-format off
#include <windows.h>

#include <shellapi.h>

#include <timeapi.h>

// clang-format on
#include <algorithm>

#include <atomic>

#include <filesystem>

#include <fstream>

#include <functional>

#include <mutex>

#include <string>

#include <string_view>

#include <vector>

#include "../common/config.h"

#include "../common/crash_handler.h"

#include "../common/logging.h"

#include "../common/monitor_selection.h"

#include "../common/process_ipc.h"

#include "../common/shared_defs.h"

#include "../common/strict_integer_parse.h"

#include "../common/vulkan_layer_registration.h"

#include "dump_helper.h"

#include "hotkey_input_hook.h"

#include "injection.h"

#include "main_vulkan_residency.h"

#include "process_loopback_worker_host.h"

#include "pseudo_overlay.h"

#include "recording_manifest.h"

#include "screenshot.h"

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

// Hotkey IDs live in common/hotkey_matcher.h so the RegisterHotKey
// registration and the low-level keyboard path cannot drift apart.

void LaunchGameSuspended(const std::string& path);

bool ConnectToChildProcesses(DWORD);

void SendCommandToAll(ProcessCommand cmd);

void PublishRecordingFailureOverlayNotification(const char* reason);

void CheckRecordingFailureState();

void ToggleRecording();

void ToggleAudioOnlyRecording();

void ToggleOverlay();

void DispatchHotkey(int hotkeyId);

void ShutdownChildProcesses();

void CheckChildProcessHealth();

bool CompleteControllerStartup();

BOOL WINAPI ControllerConsoleHandler(DWORD ctrlType);

int ControllerMain(HINSTANCE hInstance);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow);

// Controller state
inline bool main_g_Running = true;

inline bool main_g_Recording = false;

inline uint32_t main_g_RecordingSerial = 0;

inline std::atomic<RecordingStartIntent> main_g_RecordingStartIntent{RecordingStartIntent::Idle};

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline AppConfig main_g_Config;

inline std::string main_g_ConfigPath;

// Auto-record feature for autonomous testing
inline bool main_g_AutoRecordEnabled = false;

inline DWORD main_g_AutoRecordDelayMs = 3000;      // Delay before starting

inline DWORD main_g_AutoRecordDurationMs = 10000;  // Recording duration

inline DWORD main_g_AutoRecordStartTime = 0;

// Deferred game launch (for --launch mode)
inline std::string main_g_DeferredLaunchPath;

// Child process handles
inline HANDLE main_g_hInjectProcess = NULL;

inline HANDLE main_g_hMediaProcess = NULL;

inline HANDLE main_g_hLimiterProcess = NULL;

inline HANDLE main_g_hLoggerProcess = NULL;

inline HANDLE main_g_hSensorProcess = NULL;

// IPC clients for child processes
inline std::unique_ptr<ProcessIPCClient> main_g_InjectClient;

inline std::unique_ptr<ProcessIPCClient> main_g_MediaClient;

inline std::unique_ptr<ProcessIPCClient> main_g_LimiterClient;

inline TrayIcon* main_g_Tray = nullptr;

inline std::unique_ptr<PseudoOverlay> main_g_PseudoOverlay;

// Hotkeys whose combination this process actually holds. Both delivery paths
// consult it, so neither serves a combination another application owns.
inline HotkeyOwnership main_g_HotkeyOwnership;

inline constexpr UINT main_kMsgCompleteControllerStartup = WM_APP + 1;

// A hotkey the low-level keyboard hook recognized and consumed. It is a message
// of its own rather than a synthesized WM_HOTKEY so the logs stay honest about
// which delivery path served a press.
inline constexpr UINT main_kMsgHotkeyFromInputHook = WM_APP + 2;

namespace {
struct ControllerStartupTimingState {
    int64_t controllerStartUs = 0;
    int64_t vulkanRegUs = 0;
    int64_t trayCreateUs = 0;
    bool complete = false;
};
}

inline ControllerStartupTimingState main_g_ControllerStartupTiming;

inline double QpcDeltaToMs(int64_t deltaUs) {
    return static_cast<double>(deltaUs) / 1000.0;
}

inline void PumpStartupMessages() {
    MSG msg = {};
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

inline void PrimeStartupCursor() {
    MSG msg = {};
    PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE);
    HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW);
    if (arrow) {
        SetCursor(arrow);
    }
}

inline bool HasExactCommandLineArgument(const wchar_t* expected) {
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments)
        return false;
    bool found = false;
    for (int index = 1; index < argumentCount; ++index) {
        if (_wcsicmp(arguments[index], expected) == 0) {
            found = true;
            break;
        }
    }
    LocalFree(reinterpret_cast<HLOCAL>(arguments));
    return found;
}

inline bool TryParseAutoRecordValue(std::string_view value, DWORD& result) {
    uint32_t parsed = 0;
    if (!ce::TryParseUInt32(value, parsed))
        return false;
    result = parsed;
    return true;
}

inline bool IsProcessRunning(HANDLE hProcess) {
    if (!hProcess) {
        return false;
    }
    DWORD exitCode = 0;
    return GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE;
}

namespace {
// Measures how long the controller's main thread spends inside a blocking section. The
// pseudo-overlay owns a dedicated message thread, but the tray and global hotkeys still
// depend on this controller thread and remain useful diagnostics when it is starved.
struct MainThreadBlockTimer {
    const char* label_;
    ULONGLONG startMs_;
    explicit MainThreadBlockTimer(const char* label) : label_(label), startMs_(GetTickCount64()) {}
    ~MainThreadBlockTimer() {
        const ULONGLONG elapsedMs = GetTickCount64() - startMs_;
        if (elapsedMs >= 250) {
            LogWarn(
                "[Controller] Main-thread blocked %llums in %s — tray + global hotkeys were unresponsive this long",
                static_cast<unsigned long long>(elapsedMs), label_);
        }
    }
};
}

inline bool ShouldStartMediaProcessAtStartup() {
    return main_g_AutoRecordEnabled;
}

inline void PrepareRecordingDiagnosticIdentity() {
    if (main_g_hMediaProcess && IsProcessRunning(main_g_hMediaProcess) && !g_RecordingId.empty())
        return;

    char recordingId[24]{};
    snprintf(recordingId, sizeof(recordingId), "r%04lu", static_cast<unsigned long>(++main_g_RecordingSerial));
    g_RecordingId = recordingId;
    LogInfo("[Controller] Recording diagnostic identity allocated: %s", g_RecordingId.c_str());
}

inline bool ShouldStartLimiterProcessAtStartup(const AppConfig& config) {
    return config.fpsLimiter.generalEnabled || (main_g_AutoRecordEnabled && config.fpsLimiter.captureSyncEnabled);
}

inline bool ShouldKeepLimiterProcessRunning(const AppConfig& config) {
    return config.fpsLimiter.generalEnabled || (main_g_Recording && config.fpsLimiter.captureSyncEnabled) ||
           (main_g_AutoRecordEnabled && config.fpsLimiter.captureSyncEnabled);
}

inline bool ShouldStartLoggerProcess(const AppConfig& config) {
    return IsAnyLoggingEnabled(config.logLevel);
}

inline bool ShouldStartSensorProcess(const AppConfig& config) {
    return config.overlay.showCPU || config.overlay.showGPU || config.overlay.showRAM || config.overlay.showVRAM;
}

inline std::vector<PseudoOverlayApplicationConfig> ResolvePseudoOverlayApplicationConfigs(const AppConfig& baseConfig) {
    std::vector<PseudoOverlayApplicationConfig> profiles;
    profiles.reserve(baseConfig.applicationProfiles.size());

    for (const ApplicationProfile& profile : baseConfig.applicationProfiles) {
        // Per-app overrides use process identity. A title-only profile can route
        // WGC/DXGI video, but it cannot safely own arbitrary setting overrides.
        if (!profile.target.HasProcess())
            continue;

        AppConfig resolvedConfig;
        LoadConfig(main_g_ConfigPath, resolvedConfig, profile.target.pattern);

        PseudoOverlayApplicationConfig overlayProfile;
        overlayProfile.section = profile.section;
        overlayProfile.processName = profile.target.pattern;
        overlayProfile.settings = resolvedConfig.pseudoOverlay;
        // process_list is a global compatibility fallback. A canonical video
        // profile is already its own warning target and does not need a second list.
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

inline void SyncPseudoOverlayConfiguration(const char* reason) {
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

inline void WriteSessionManifest(const std::string& logsDir, const AppConfig& config, ProcessMode mode) {
    std::ofstream manifest(logsDir + "\\session_manifest.txt", std::ios::out | std::ios::trunc);
    if (!manifest.is_open()) {
        return;
    }

    manifest << "build_version=" << GetCaptureVersion() << "\n";
    manifest << "build_timestamp=" << GetBuildTimestamp() << "\n";
    manifest << "session_dir=" << logsDir << "\n";
    manifest << "process_mode="
             << (mode == ProcessMode::Controller ? "Controller"
                 : mode == ProcessMode::Inject   ? "Inject"
                 : mode == ProcessMode::Media    ? "Media"
                 : mode == ProcessMode::Limiter  ? "Limiter"
                 : mode == ProcessMode::Logger   ? "Logger"
                 : mode == ProcessMode::Sensors  ? "Sensors"
                                                 : "Unknown")
             << "\n";
    manifest << "log_level=" << LogLevelToConfigString(config.logLevel) << "\n";
    manifest << "capture_method=" << config.captureMethod << "\n";
    manifest << "capture_monitor=" << config.captureMonitor << "\n";
    manifest << "overlay_enabled=" << (config.overlay.showOverlay ? 1 : 0) << "\n";
    manifest << "overlay_observer_only=" << (config.overlay.observerOnly ? 1 : 0) << "\n";
    manifest << "overlay_observer_policy_only=" << (config.overlay.observerPolicyOnly ? 1 : 0) << "\n";
    manifest << "overlay_observer_startup_present_only=" << (config.overlay.observerStartupPresentOnly ? 1 : 0) << "\n";
    manifest << "steam_overlay_loaded=0\n";
    manifest << "streamline_loaded=0\n";
    manifest << "ffx_loaded=0\n";
    manifest << "fg_shadow_state_enabled=1\n";
    manifest << "fg_state_schema_version=1\n";
    manifest << "logger_enabled=" << (ShouldStartLoggerProcess(config) ? 1 : 0) << "\n";
    manifest << "sensor_enabled=" << (ShouldStartSensorProcess(config) ? 1 : 0) << "\n";
    manifest << "game_whitelist_entries=" << config.gameWhitelist.size() << "\n";
    manifest << "overlay_whitelist_entries=" << config.overlayWhitelist.size() << "\n";
    manifest << "logs=" << GetLogFileName(mode) << "\n";
    manifest << "media_logs=media_*.log\n";
    manifest << "recording_manifests=recording_*.manifest\n";
    manifest << "notes=Use this file as the compact session entrypoint before reading detailed logs.\n";
}

inline void WriteRecordingManifest(const std::string& logsDir, const AppConfig& config, const std::string& mediaLog) {
    if (g_RecordingId.empty())
        return;

    const DWORD processId = GetCurrentProcessId();
    const std::string path = logsDir + "\\recording_" + g_RecordingId + "_" + std::to_string(processId) +
                             ".manifest";
    std::ofstream manifest(path, std::ios::out | std::ios::trunc);
    if (!manifest.is_open())
        return;

    manifest << "build_version=" << GetCaptureVersion() << "\n";
    manifest << "build_timestamp=" << GetBuildTimestamp() << "\n";
    manifest << "recording_id=" << g_RecordingId << "\n";
    manifest << "media_pid=" << processId << "\n";
    manifest << "media_log=" << mediaLog << "\n";
    manifest << "base_capture_method=" << config.captureMethod << "\n";
    manifest << "base_capture_monitor=" << config.captureMonitor << "\n";
    manifest << "status=media_process_started\n";
    manifest << "notes=Recording-specific evidence; correlate by recording_id and media_pid.\n";
}

inline DWORD GetControllerLoopWaitMs(DWORD lastConfigCheck) {
    DWORD waitMs = 2000;
    DWORD now = GetTickCount();

    DWORD configElapsed = now - lastConfigCheck;
    if (configElapsed >= 1000) {
        return 0;
    }
    DWORD configWaitMs = 1000 - configElapsed;
    if (configWaitMs < waitMs) {
        waitMs = configWaitMs;
    }

    if (main_g_AutoRecordEnabled && main_g_AutoRecordStartTime > 0) {
        DWORD elapsed = now - main_g_AutoRecordStartTime;
        DWORD nextAutoActionMs = !main_g_Recording ? main_g_AutoRecordDelayMs : (main_g_AutoRecordDelayMs + main_g_AutoRecordDurationMs);
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

inline bool HotkeyConfigEquals(const AppConfig::HotkeyConfig& a, const AppConfig::HotkeyConfig& b) {
    return a.vkey == b.vkey && a.ctrl == b.ctrl && a.shift == b.shift && a.alt == b.alt && a.win == b.win;
}

inline void CloseProcessHandle(HANDLE& processHandle) {
    if (processHandle) {
        CloseHandle(processHandle);
        processHandle = NULL;
    }
}

inline bool EnsureChildProcessConnected(ProcessMode mode, HANDLE& processHandle, ProcessIPCClient* client, DWORD timeoutMs,
                                 const char* processName) {
    if (processHandle && IsProcessRunning(processHandle) && client && !client->IsConnected()) {
        const DWORD disconnectWaitMs = std::min<DWORD>(timeoutMs, 2000);
        const ULONGLONG deadline = GetTickCount64() + disconnectWaitMs;
        while (IsProcessRunning(processHandle) && GetTickCount64() < deadline) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline)
                break;
            const ULONGLONG remaining64 = deadline - now;
            const DWORD remaining = static_cast<DWORD>(std::min<ULONGLONG>(remaining64, MAXDWORD));
            const DWORD wait =
                MsgWaitForMultipleObjectsEx(1, &processHandle, remaining, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (wait == WAIT_OBJECT_0)
                break;
            if (wait == WAIT_OBJECT_0 + 1)
                PumpStartupMessages();
            else
                break;
        }
        if (IsProcessRunning(processHandle)) {
            LogWarn("[Controller] %s has not exited after its IPC channel broke; deferring respawn", processName);
            return false;
        }
    }
    if (!processHandle || !IsProcessRunning(processHandle)) {
        if (client) {
            client->Disconnect();
        }
        if (processHandle) {
            CloseHandle(processHandle);
            processHandle = NULL;
        }

        const int64_t spawnStartUs = Log_GetQpcUs();
        processHandle = SpawnChildProcess(mode, main_g_ConfigPath.c_str(), client);
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

    LogError("[Controller] %s process is running without its inherited authenticated IPC channel", processName);
    return false;
}

inline bool EnsureMediaProcessReady(DWORD timeoutMs) {
    return EnsureChildProcessConnected(ProcessMode::Media, main_g_hMediaProcess, main_g_MediaClient.get(), timeoutMs, "media");
}

inline bool EnsureLimiterProcessReady(DWORD timeoutMs) {
    return EnsureChildProcessConnected(ProcessMode::Limiter, main_g_hLimiterProcess, main_g_LimiterClient.get(), timeoutMs,
                                       "limiter");
}

inline bool ShutdownIpcChildProcess(HANDLE& processHandle, ProcessIPCClient* client, const char* processName,
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

inline void SyncLimiterProcess(const AppConfig& config) {
    if (ShouldKeepLimiterProcessRunning(config)) {
        EnsureLimiterProcessReady(10000);
        return;
    }

    if (main_g_hLimiterProcess) {
        LogInfo("[Controller] Limiter no longer needed; shutting it down");
        ShutdownIpcChildProcess(main_g_hLimiterProcess, main_g_LimiterClient.get(), "limiter", 5000);
    } else if (main_g_LimiterClient) {
        main_g_LimiterClient->Disconnect();
    }
}

inline void SyncLoggerAndSensorProcesses(const AppConfig& config) {
    const bool wantLogger = ShouldStartLoggerProcess(config);
    const bool wantSensor = ShouldStartSensorProcess(config);
    const bool loggerRunning = IsProcessRunning(main_g_hLoggerProcess);
    const bool sensorRunning = IsProcessRunning(main_g_hSensorProcess);

    if (loggerRunning == wantLogger && sensorRunning == wantSensor) {
        if (!loggerRunning) {
            CloseProcessHandle(main_g_hLoggerProcess);
        }
        if (!sensorRunning) {
            CloseProcessHandle(main_g_hSensorProcess);
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

    if (main_g_hLoggerProcess) {
        WaitForSingleObject(main_g_hLoggerProcess, 5000);
        CloseProcessHandle(main_g_hLoggerProcess);
    }
    if (main_g_hSensorProcess) {
        WaitForSingleObject(main_g_hSensorProcess, 5000);
        CloseProcessHandle(main_g_hSensorProcess);
    }

    if (hShutdownEvent) {
        ResetEvent(hShutdownEvent);
        CloseHandle(hShutdownEvent);
    }

    if (wantLogger) {
        main_g_hLoggerProcess = SpawnChildProcess(ProcessMode::Logger, main_g_ConfigPath.c_str());
        if (!main_g_hLoggerProcess) {
            LogError("[Controller] Failed to restart logger process");
        }
    }
    if (wantSensor) {
        main_g_hSensorProcess = SpawnChildProcess(ProcessMode::Sensors, main_g_ConfigPath.c_str());
        if (!main_g_hSensorProcess) {
            LogError("[Controller] Failed to restart sensor process");
        }
    }
}

// Remove old session directories from logs/, keeping the most recent maxKeep.
// Also cleans up any stale flat .log/.csv files from pre-session-dir versions.
inline void CleanupOldSessionDirs(const std::string& logsDir, size_t maxKeep = 20) {
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

namespace {
struct DeferredLaunchCommand {
    std::string rawCommandLine;
    std::string executablePath;
    std::string workingDirectory;
    std::string fileName;
};
}

// Helper: open inject's shared memory and run a callback with a writable view.
// Returns true if the shared memory was opened and the callback executed.
inline bool WithInjectSharedMem(const std::function<void(SharedMemoryLayout*)>& fn) {
    HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (!hDisc)
        return false;
    DiscoveryInfo* pDisc = (DiscoveryInfo*)MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
    if (!ValidateDiscoveryInfo(pDisc)) {
        if (pDisc)
            UnmapViewOfFile(pDisc);
        CloseHandle(hDisc);
        return false;
    }
    uint32_t injPid = pDisc->GetInjectPid();
    UnmapViewOfFile(pDisc);
    CloseHandle(hDisc);
    if (injPid == 0)
        return false;

    wchar_t shmName[64];
    GenerateSharedMemName(shmName, 64, injPid);
    HANDLE hShm = OpenFileMappingW(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, shmName);
    if (!hShm)
        return false;
    auto* pShm =
        (SharedMemoryLayout*)MapViewOfFile(hShm, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, sizeof(SharedMemoryLayout));
    if (!pShm) {
        CloseHandle(hShm);
        return false;
    }
    if (!ValidateSharedMemory(pShm)) {
        LogError("[Controller] Rejected inject shared memory with incompatible ABI (version=%u size=%u abi=0x%08X)",
                 pShm->GetVersion(), pShm->structSize.load(std::memory_order_acquire),
                 pShm->abiSignature.load(std::memory_order_acquire));
        UnmapViewOfFile(pShm);
        CloseHandle(hShm);
        return false;
    }
    fn(pShm);
    UnmapViewOfFile(pShm);
    CloseHandle(hShm);
    return true;
}

inline bool PublishRecordingStartIntent(RecordingStartIntent intent, const char* reason) {
    main_g_RecordingStartIntent.store(intent, std::memory_order_release);
    const bool published = WithInjectSharedMem([&](SharedMemoryLayout* sharedMemory) {
        sharedMemory->runtimeState.SetRecordingStartIntent(intent);
        if (intent != RecordingStartIntent::AudioOnly) {
            sharedMemory->runtimeState.audioOnly.store(false, std::memory_order_release);
        } else {
            sharedMemory->runtimeState.audioOnly.store(true, std::memory_order_release);
        }
    });
    if (main_g_PseudoOverlay) {
        main_g_PseudoOverlay->SetRecordingStartIntent(intent);
    }
    LogInfo("[Controller] Recording start intent=%s published=%d reason=%s",
            intent == RecordingStartIntent::Video       ? "video"
            : intent == RecordingStartIntent::AudioOnly ? "audio-only"
                                                        : "idle",
            published ? 1 : 0, reason ? reason : "unspecified");
    return published;
}

inline bool RequestChildRecordingStop(ProcessIPCClient* client, const char* childName, const char* reason,
                                      DWORD timeoutMs) {
    if (!client || !client->IsConnected())
        return false;

    ProcessResponse response = ProcessResponse::Error;
    if (!client->SendCommand(ProcessCommand::StopRecording, nullptr, &response, timeoutMs) ||
        response == ProcessResponse::Error) {
        LogWarn("[Controller] %s did not accept the recording stop (%s)", childName,
                reason ? reason : "unspecified");
        return false;
    }

    LogInfo("[Controller] %s accepted the recording stop (%s)", childName, reason ? reason : "unspecified");
    return true;
}

inline bool RequestRecordingStopAndReleaseMedia(const char* reason, DWORD timeoutMs) {
    // Ask media first. It acknowledges before finalization, so controller UI work
    // does not wait for trailer writing or the post-mux probe. Media clears the
    // hook-facing shared state before acknowledging. The inject command is only a
    // fallback when the private media channel cannot accept the request.
    const bool mediaAccepted = RequestChildRecordingStop(main_g_MediaClient.get(), "Media", reason, timeoutMs);
    const bool stopAccepted =
        mediaAccepted || RequestChildRecordingStop(main_g_InjectClient.get(), "Inject fallback", reason, timeoutMs);
    if (!stopAccepted) {
        LogWarn("[Controller] No recording child accepted the stop (%s); process teardown is the final fallback",
                reason ? reason : "unspecified");
    }

    // Media self-exits after finalization. Drop the controller's reference now so
    // the next recording creates a fresh authenticated child.
    if (main_g_MediaClient)
        main_g_MediaClient->Disconnect();
    CloseProcessHandle(main_g_hMediaProcess);
    return stopAccepted;
}
