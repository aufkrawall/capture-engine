// CaptureEngine Multi-Process Architecture
// Main entry point - acts as Controller when run without --mode flag
// Dispatches to Inject, Media, or Limiter process based on --mode=<mode>

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
#include "injection.h"
#include "process_loopback_worker_host.h"
#include "pseudo_overlay.h"
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

// Hotkey IDs
#define HOTKEY_ID_RECORD 1
#define HOTKEY_ID_SCREENSHOT 2
#define HOTKEY_ID_AUDIO_ONLY 3

// Controller state
static bool g_Running = true;
static bool g_Recording = false;
static uint32_t g_RecordingSerial = 0;
static std::atomic<RecordingStartIntent> g_RecordingStartIntent{RecordingStartIntent::Idle};
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

bool HasExactCommandLineArgument(const wchar_t* expected) {
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
    LocalFree(arguments);
    return found;
}

bool TryParseAutoRecordValue(std::string_view value, DWORD& result) {
    uint32_t parsed = 0;
    if (!ce::TryParseUInt32(value, parsed))
        return false;
    result = parsed;
    return true;
}

bool IsProcessRunning(HANDLE hProcess) {
    if (!hProcess) {
        return false;
    }
    DWORD exitCode = 0;
    return GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE;
}

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

bool ShouldStartMediaProcessAtStartup() {
    return g_AutoRecordEnabled;
}

void PrepareRecordingDiagnosticIdentity() {
    if (g_hMediaProcess && IsProcessRunning(g_hMediaProcess) && !g_RecordingId.empty())
        return;

    char recordingId[24]{};
    snprintf(recordingId, sizeof(recordingId), "r%04lu", static_cast<unsigned long>(++g_RecordingSerial));
    g_RecordingId = recordingId;
    LogInfo("[Controller] Recording diagnostic identity allocated: %s", g_RecordingId.c_str());
}

bool ShouldStartLimiterProcessAtStartup(const AppConfig& config) {
    return config.fpsLimiter.generalEnabled || (g_AutoRecordEnabled && config.fpsLimiter.captureSyncEnabled);
}

bool ShouldKeepLimiterProcessRunning(const AppConfig& config) {
    return config.fpsLimiter.generalEnabled || (g_Recording && config.fpsLimiter.captureSyncEnabled) ||
           (g_AutoRecordEnabled && config.fpsLimiter.captureSyncEnabled);
}

bool ShouldStartLoggerProcess(const AppConfig& config) {
    return IsAnyLoggingEnabled(config.logLevel);
}

bool ShouldStartSensorProcess(const AppConfig& config) {
    return config.overlay.showCPU || config.overlay.showGPU || config.overlay.showRAM || config.overlay.showVRAM;
}

std::vector<PseudoOverlayApplicationConfig> ResolvePseudoOverlayApplicationConfigs(const AppConfig& baseConfig) {
    std::vector<PseudoOverlayApplicationConfig> profiles;
    profiles.reserve(baseConfig.applicationProfiles.size());

    for (const ApplicationProfile& profile : baseConfig.applicationProfiles) {
        // Per-app overrides use process identity. A title-only profile can route
        // WGC/DXGI video, but it cannot safely own arbitrary setting overrides.
        if (!profile.target.HasProcess())
            continue;

        AppConfig resolvedConfig;
        LoadConfig(g_ConfigPath, resolvedConfig, profile.target.pattern);

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

void SyncPseudoOverlayConfiguration(const char* reason) {
    std::vector<PseudoOverlayApplicationConfig> profiles = ResolvePseudoOverlayApplicationConfigs(g_Config);
    const bool anyProfileEnabled =
        std::any_of(profiles.begin(), profiles.end(), [](const PseudoOverlayApplicationConfig& profile) {
            return profile.settings.enabled;
        });

    if (!g_PseudoOverlay && !g_Config.pseudoOverlay.enabled && !anyProfileEnabled)
        return;

    if (!g_PseudoOverlay) {
        LogInfo("[Controller] Initializing pseudo-overlay (%s)...", reason ? reason : "configuration");
        auto overlay = std::make_unique<PseudoOverlay>();
        overlay->UpdateConfig(g_Config.pseudoOverlay, profiles);
        overlay->SetRecordingStartIntent(g_RecordingStartIntent.load(std::memory_order_acquire));
        HMODULE hMod = GetModuleHandle(NULL);
        if (!overlay->Init(reinterpret_cast<HINSTANCE>(hMod))) {
            LogError("[Controller] Failed to initialize pseudo-overlay");
            return;
        }
        g_PseudoOverlay = std::move(overlay);
        LogInfo("[Controller] Pseudo-overlay initialized");
        return;
    }

    g_PseudoOverlay->UpdateConfig(g_Config.pseudoOverlay, profiles);
}

void WriteSessionManifest(const std::string& logsDir, const AppConfig& config, ProcessMode mode) {
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

void WriteRecordingManifest(const std::string& logsDir, const AppConfig& config, const std::string& mediaLog) {
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

DWORD GetControllerLoopWaitMs(DWORD lastConfigCheck) {
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
        processHandle = SpawnChildProcess(mode, g_ConfigPath.c_str(), client);
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

namespace {
struct DeferredLaunchCommand {
    std::string rawCommandLine;
    std::string executablePath;
    std::string workingDirectory;
    std::string fileName;
};

std::string TrimCommandWhitespace(const std::string& value) {
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }

    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

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
}  // namespace

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
